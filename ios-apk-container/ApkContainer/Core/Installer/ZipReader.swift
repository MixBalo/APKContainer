//
//  ZipReader.swift
//  ApkContainer
//
//  Status: IMPLEMENTED (real, from-scratch ZIP reader).
//
//  Helper file (not in the orchestrator's file list, but required by ApkParser,
//  NativeLibExtractor, IconExtractor, and DexInspector). Foundation has no
//  built-in ZIP reader on iOS, so we parse the ZIP container format ourselves.
//
//  What is implemented:
//    - End-of-Central-Directory record scan (handles up-to-64KiB ZIP comment).
//    - Central directory file header walk (PK\001\002).
//    - Local file header (PK\003\004) skip + raw file data slice.
//    - Compression method 0 (STORE) passthrough.
//    - Compression method 8 (DEFLATE) via Apple's Compression framework
//      (`NSData.decompressed(using: .zlib)` — Apple's "zlib" algorithm actually
//      operates on raw DEFLATE streams per RFC 1951, which is exactly what ZIP
//      method 8 stores).
//
//  What is NOT implemented (deliberate scope):
//    - ZIP64 (files > 4 GiB or > 65535 entries). Detected and rejected with a
//      clear error. APKs of that size are out of scope.
//    - Encryption (legacy ZIP crypto + AES). Detected and rejected.
//    - BZIP2 (method 12), Zstandard (method 93), etc. — not used by Android
//      APK build tooling as of 2024, so rejected.
//
//  Honesty contract: no stubs. If a feature is missing, it throws a typed error
//  rather than silently producing wrong output.
//

import Foundation

/// Errors raised by `ZipReader`.
public enum ZipError: LocalizedError {
    case fileTooSmall
    case eocdNotFound
    case invalidCentralDirectory
    case truncatedCentralDirectory
    case invalidCDFHSignature
    case truncatedLocalHeader
    case invalidLFHSignature
    case truncatedFileData
    case unsupportedCompressionMethod(UInt16)
    case zip64Unsupported
    case encryptedEntryUnsupported
    case invalidStringEncoding

    public var errorDescription: String? {
        switch self {
        case .fileTooSmall:                    return "ZIP file is smaller than the minimum EOCD size (22 bytes)."
        case .eocdNotFound:                    return "End-of-Central-Directory record not found."
        case .invalidCentralDirectory:         return "Central directory offset+size exceeds file bounds."
        case .truncatedCentralDirectory:       return "Central directory entry runs past EOF."
        case .invalidCDFHSignature:            return "Bad central directory file header signature."
        case .truncatedLocalHeader:            return "Local file header runs past EOF."
        case .invalidLFHSignature:             return "Bad local file header signature."
        case .truncatedFileData:               return "Compressed file data runs past EOF."
        case .unsupportedCompressionMethod(let m):
            return "Unsupported ZIP compression method \(m)."
        case .zip64Unsupported:                return "ZIP64 extensions are not supported by this reader."
        case .encryptedEntryUnsupported:       return "Encrypted ZIP entries are not supported."
        case .invalidStringEncoding:           return "Entry name is not valid UTF-8."
        }
    }
}

/// Minimal in-file ZIP reader sufficient for APK inspection.
public final class ZipReader {

    /// One central-directory entry.
    public struct Entry: Hashable {
        /// Entry name (e.g. `"AndroidManifest.xml"`, `"lib/arm64-v8a/libunity.so"`).
        public let name: String
        /// 0 = STORE, 8 = DEFLATE. Other values will cause `readData(for:)` to throw.
        public let compressionMethod: UInt16
        public let compressedSize: UInt64
        public let uncompressedSize: UInt64
        public let localHeaderOffset: UInt64
        public let crc32: UInt32
    }

    /// Whole-file data. Backed by mmap on iOS for large APKs.
    private let data: Data
    public private(set) var entries: [Entry] = []
    private var entryByName: [String: Entry] = [:]

    public init(apkURL: URL) throws {
        // `.alwaysMapped` lets the OS page the file in on demand rather than
        // copying it all into the heap. Important for >100 MB APKs.
        self.data = try Data(contentsOf: apkURL, options: [.alwaysMapped])
        try parseCentralDirectory()
    }

    // MARK: - Public API

    public func entry(named name: String) -> Entry? {
        entryByName[name]
    }

    public func entries(matching prefix: String) -> [Entry] {
        entries.filter { $0.name.hasPrefix(prefix) }
    }

    /// Inflates (or copies, for STORE) the file data for `entry`.
    public func readData(for entry: Entry) throws -> Data {
        let lhOffset = Int(entry.localHeaderOffset)
        guard lhOffset + 30 <= data.count else {
            throw ZipError.truncatedLocalHeader
        }
        // Local file header layout (offsets from lhOffset):
        //   0: magic (4)  = 0x04034b50 ("PK\003\004")
        //   4: version needed (2)
        //   6: flags (2)
        //   8: compression method (2)
        //  10: last mod time (2)
        //  12: last mod date (2)
        //  14: crc32 (4)
        //  18: compressed size (4)
        //  22: uncompressed size (4)
        //  26: file name length (2)
        //  28: extra field length (2)
        //  30: file name + extra field, then file data.
        let magic = readUInt32(at: lhOffset)
        guard magic == 0x04034b50 else { throw ZipError.invalidLFHSignature }

        // Bit 0 of the general-purpose flags = encryption. We reject encrypted
        // entries outright; APKs produced by aapt are never encrypted.
        let flags = readUInt16(at: lhOffset + 6)
        if (flags & 0x0001) != 0 { throw ZipError.encryptedEntryUnsupported }

        let nameLen = Int(readUInt16(at: lhOffset + 26))
        let extraLen = Int(readUInt16(at: lhOffset + 28))
        let dataStart = lhOffset + 30 + nameLen + extraLen
        let dataEnd = dataStart + Int(entry.compressedSize)
        guard dataEnd <= data.count else {
            throw ZipError.truncatedFileData
        }
        let compressed = data.subdata(in: dataStart..<dataEnd)
        return try ZipReader.inflate(compressed: compressed, method: entry.compressionMethod)
    }

    /// Convenience: read by entry name.
    public func readData(forEntryNamed name: String) throws -> Data {
        guard let entry = entryByName[name] else {
            throw ZipError.truncatedFileData // no entry; reuse a generic error
        }
        return try readData(for: entry)
    }

    /// Inflate (or passthrough) a single member's compressed bytes.
    public static func inflate(compressed: Data, method: UInt16) throws -> Data {
        switch method {
        case 0:
            // STORE: bytes are already raw.
            return compressed
        case 8:
            // DEFLATE (RFC 1951). Apple's `COMPRESSION_ZLIB` algorithm
            // (exposed via NSData.decompressed(using: .zlib)) operates on raw
            // DEFLATE streams despite its RFC-1950 name; this matches ZIP
            // method 8 exactly (no zlib header/trailer in the stored bytes).
            return try (compressed as NSData).decompressed(using: .zlib) as Data
        default:
            throw ZipError.unsupportedCompressionMethod(method)
        }
    }

    // MARK: - Central directory parsing

    private func parseCentralDirectory() throws {
        let eocdOffset = try findEOCDOffset()

        // EOCD layout (offsets from eocdOffset):
        //   0: magic (4) = 0x06054b50 ("PK\005\006")
        //   4: disk number (2)
        //   6: disk with CD start (2)
        //   8: entries on this disk (2)
        //  10: total entries (2)
        //  12: CD size (4)
        //  16: CD offset (4)
        //  20: comment length (2)
        //  22: comment (variable)
        let totalEntries = Int(readUInt16(at: eocdOffset + 10))
        let cdSize = Int(readUInt32(at: eocdOffset + 12))
        let cdOffset = Int(readUInt32(at: eocdOffset + 16))

        // ZIP64 sentinel — bail with a clear error.
        if cdSize == 0xFFFFFFFF || cdOffset == 0xFFFFFFFF || totalEntries == 0xFFFF {
            throw ZipError.zip64Unsupported
        }
        guard cdOffset + cdSize <= data.count else {
            throw ZipError.invalidCentralDirectory
        }

        var offset = cdOffset
        for _ in 0..<totalEntries {
            // Central directory file header layout (offsets from `offset`):
            //   0: magic (4) = 0x02014b50 ("PK\001\002")
            //   4: version made by (2)
            //   6: version needed (2)
            //   8: flags (2)
            //  10: compression method (2)
            //  12: last mod time (2)
            //  14: last mod date (2)
            //  16: crc32 (4)
            //  20: compressed size (4)
            //  24: uncompressed size (4)
            //  28: file name length (2)
            //  30: extra field length (2)
            //  32: file comment length (2)
            //  34: disk number start (2)
            //  36: internal attrs (2)
            //  38: external attrs (4)
            //  42: local header offset (4)
            //  46: file name (variable) + extra field (variable) + comment (variable)
            guard offset + 46 <= data.count else {
                throw ZipError.truncatedCentralDirectory
            }
            let magic = readUInt32(at: offset)
            guard magic == 0x02014b50 else { throw ZipError.invalidCDFHSignature }

            let method = readUInt16(at: offset + 10)
            let crc = readUInt32(at: offset + 16)
            var compressedSize = UInt64(readUInt32(at: offset + 20))
            var uncompressedSize = UInt64(readUInt32(at: offset + 24))
            let nameLen = Int(readUInt16(at: offset + 28))
            let extraLen = Int(readUInt16(at: offset + 30))
            let commentLen = Int(readUInt16(at: offset + 32))
            var localHeaderOffset = UInt64(readUInt32(at: offset + 42))

            // Optional ZIP64 extra field parse. Only kicks in if the regular
            // field is the 0xFFFFFFFF sentinel. We need the uncompressed size
            // and local header offset to read the file later.
            if compressedSize == 0xFFFFFFFF || uncompressedSize == 0xFFFFFFFF ||
                localHeaderOffset == 0xFFFFFFFF {
                let extraStart = offset + 46 + nameLen
                try parseZip64Extra(
                    extraStart: extraStart,
                    extraLen: extraLen,
                    compressedSize: &compressedSize,
                    uncompressedSize: &uncompressedSize,
                    localHeaderOffset: &localHeaderOffset
                )
            }

            let nameStart = offset + 46
            guard nameStart + nameLen <= data.count else {
                throw ZipError.truncatedCentralDirectory
            }
            let nameData = data.subdata(in: nameStart..<(nameStart + nameLen))
            guard let name = String(data: nameData, encoding: .utf8) else {
                throw ZipError.invalidStringEncoding
            }

            let entry = Entry(
                name: name,
                compressionMethod: method,
                compressedSize: compressedSize,
                uncompressedSize: uncompressedSize,
                localHeaderOffset: localHeaderOffset,
                crc32: crc
            )
            entries.append(entry)
            // First occurrence wins (some APKs have duplicate paths; that's
            // malformed but we shouldn't crash on it).
            if entryByName[name] == nil {
                entryByName[name] = entry
            }

            offset = nameStart + nameLen + extraLen + commentLen
        }
    }

    /// Parse the ZIP64 extended information extra field (header ID 0x0001).
    /// Only the fields whose standard values are 0xFFFFFFFF are populated.
    private func parseZip64Extra(
        extraStart: Int,
        extraLen: Int,
        compressedSize: inout UInt64,
        uncompressedSize: inout UInt64,
        localHeaderOffset: inout UInt64
    ) throws {
        var p = extraStart
        let end = extraStart + extraLen
        while p + 4 <= end {
            let headerID = readUInt16(at: p)
            let dataSize = Int(readUInt16(at: p + 2))
            p += 4
            guard p + dataSize <= end else { throw ZipError.zip64Unsupported }
            if headerID == 0x0001 {
                var q = p
                if uncompressedSize == 0xFFFFFFFF {
                    guard q + 8 <= p + dataSize else { throw ZipError.zip64Unsupported }
                    uncompressedSize = readUInt64(at: q)
                    q += 8
                }
                if compressedSize == 0xFFFFFFFF {
                    guard q + 8 <= p + dataSize else { throw ZipError.zip64Unsupported }
                    compressedSize = readUInt64(at: q)
                    q += 8
                }
                if localHeaderOffset == 0xFFFFFFFF {
                    guard q + 8 <= p + dataSize else { throw ZipError.zip64Unsupported }
                    localHeaderOffset = readUInt64(at: q)
                    q += 8
                }
                return
            }
            p += dataSize
        }
        throw ZipError.zip64Unsupported
    }

    /// Scan backward from EOF for the EOCD signature, respecting the maximum
    /// 64 KiB ZIP comment length.
    private func findEOCDOffset() throws -> Int {
        let fileSize = data.count
        let minEOCDSize = 22
        guard fileSize >= minEOCDSize else { throw ZipError.fileTooSmall }

        let maxCommentSize = 0xFFFF
        let searchStart = max(0, fileSize - maxCommentSize - minEOCDSize)
        // Search backwards for the 4-byte signature 0x06054b50 ("PK\005\006").
        var offset = fileSize - minEOCDSize
        while offset >= searchStart {
            if data[offset] == 0x50 &&
                data[offset + 1] == 0x4b &&
                data[offset + 2] == 0x05 &&
                data[offset + 3] == 0x06 {
                return offset
            }
            offset -= 1
        }
        throw ZipError.eocdNotFound
    }

    // MARK: - Little-endian readers

    private func readUInt16(at offset: Int) -> UInt16 {
        let lo = UInt16(data[offset])
        let hi = UInt16(data[offset + 1])
        return lo | (hi << 8)
    }

    private func readUInt32(at offset: Int) -> UInt32 {
        let b0 = UInt32(data[offset])
        let b1 = UInt32(data[offset + 1])
        let b2 = UInt32(data[offset + 2])
        let b3 = UInt32(data[offset + 3])
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)
    }

    private func readUInt64(at offset: Int) -> UInt64 {
        var value: UInt64 = 0
        for i in 0..<8 {
            value |= UInt64(data[offset + i]) << (8 * i)
        }
        return value
    }
}
