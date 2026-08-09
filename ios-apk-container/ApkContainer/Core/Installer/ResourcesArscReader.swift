//
//  ResourcesArscReader.swift
//  ApkContainer
//
//  Status: PARTIAL.
//    - Real: RES_TABLE_TYPE header read + global string pool parse + raw
//      string lookup by index.
//    - STUB: full entry→value resolution across configurations (needed to
//      resolve @string/app_name, @drawable/ic_launcher, etc.). Returns nil
//      from all reference-resolving APIs.
//
//  Why partial: resources.arsc is a substantial format (RES_TABLE_TYPE →
//  RES_TABLE_PACKAGE_TYPE → RES_TABLE_TYPE_SPEC_TYPE / RES_TABLE_TYPE_TYPE →
//  entries → values across configurations, plus complex value types). Building
//  a real resolver is a meaningful body of work; for the v1 of APKLive we
//  instead fall back to:
//    - Literal label strings from the manifest AXML (when present).
//    - Heuristic icon paths (res/mipmap-xxhdpi-v4/ic_launcher.png, etc.).
//  See `IconExtractor` for the heuristic implementation.
//
//  Honesty contract: every STUB is marked and explained.
//

import Foundation

/// Minimal `resources.arsc` reader.
///
/// All reference-resolution APIs are STUBs (return nil) and are kept on the
/// type so callers don't have to change when a real resolver is added later.
public enum ResourcesArscReader {

    // MARK: - Real: raw string pool

    /// Returns the raw string at `index` in the global string pool of `arsc`.
    /// Returns nil if `arsc` is malformed or `index` is out of range.
    public static func rawString(arsc: Data, index: Int) -> String? {
        guard let pool = parseGlobalStringPool(arsc) else { return nil }
        guard index >= 0 && index < pool.count else { return nil }
        return pool[index]
    }

    /// Parses the RES_TABLE_TYPE header and the immediately-following global
    /// string pool, returning the array of strings. Returns nil on any error.
    ///
    /// RES_TABLE_TYPE layout:
    ///   0: chunk type (UInt16) = 0x0002
    ///   2: chunk header size (UInt16) = 0x000C
    ///   4: chunk size (UInt32)
    ///   8: package count (UInt32)
    ///   12: first package chunk OR a global string pool chunk
    ///
    /// The global string pool is a standard RES_STRING_POOL_TYPE chunk (same
    /// layout as in AXML). We reuse the same parsing logic.
    public static func parseGlobalStringPool(_ arsc: Data) -> [String]? {
        guard arsc.count >= 12 else { return nil }
        let type = readUInt16(arsc, at: 0)
        guard type == 0x0002 else { return nil }
        // The first sub-chunk after the table header is the global string pool.
        let poolStart = 12
        guard poolStart + 8 <= arsc.count else { return nil }
        let poolType = readUInt16(arsc, at: poolStart)
        guard poolType == 0x0001 else { return nil }
        let poolSize = Int(readUInt32(arsc, at: poolStart + 4))
        guard poolStart + poolSize <= arsc.count else { return nil }
        return parseStringPool(arsc, chunkStart: poolStart, chunkSize: poolSize)
    }

    // MARK: - STUB: reference resolution

    /// STUB: resolving `key` (e.g. `"app_name"`) to its string value inside
    /// `packageId` requires walking the package's type/key string pools, the
    /// type-spec chunks, the type chunks, finding the matching entry across
    /// configurations, and resolving the entry's typed value (which may itself
    /// be a reference). Not implemented in v1.
    public static func resolveString(arsc: Data, packageId: Int, key: String) -> String? {
        // STUB: full resources.arsc entry→value resolution is not implemented.
        return nil
    }

    /// STUB: extracting the human-readable app label requires resolving the
    /// manifest's `android:label` reference (e.g. `@string/app_name`) against
    /// resources.arsc. Not implemented in v1; the installer falls back to a
    /// literal label from the manifest (if present) or `"[unknown label]"`.
    public static func extractLabel(arscData: Data, manifest: ApkManifest) -> String? {
        // STUB: requires reference resolution, which needs the full RES_TABLE
        // entry/value table. See header.
        return nil
    }

    /// STUB: returns the on-disk path (inside the APK ZIP) of the entry
    /// referenced by `iconRef`, plus its density bucket. Not implemented.
    public static func iconEntry(
        arsc: Data,
        packageName: String,
        iconRef: UInt32
    ) -> (path: String, density: Int)? {
        // STUB: requires walking the package's type table for type "drawable"
        // / "mipmap" and resolving the entry. Not implemented in v1.
        return nil
    }

    /// STUB: best-effort icon path resolution. Not implemented in v1;
    /// `IconExtractor` uses heuristic paths instead.
    public static func extractBestIconPath(
        arsc: Data,
        packageName: String
    ) -> (path: String, density: Int)? {
        // STUB: same reason as `iconEntry` — requires the full RES_TABLE entry
        // table. Returns nil so the caller can fall back to heuristics.
        return nil
    }

    // MARK: - Internal string-pool parser (same layout as AXML)

    private static func parseStringPool(_ data: Data, chunkStart: Int, chunkSize: Int) -> [String]? {
        guard chunkStart + 28 <= data.count else { return nil }
        let stringCount = Int(readUInt32(data, at: chunkStart + 8))
        let flags = readUInt32(data, at: chunkStart + 16)
        let isUTF8 = (flags & (1 << 8)) != 0
        let stringsStart = Int(readUInt32(data, at: chunkStart + 20))

        var offsets: [Int] = []
        offsets.reserveCapacity(stringCount)
        for i in 0..<stringCount {
            let fieldOff = chunkStart + 28 + i * 4
            guard fieldOff + 4 <= data.count else { return nil }
            offsets.append(Int(readUInt32(data, at: fieldOff)))
        }

        var out: [String] = []
        out.reserveCapacity(stringCount)
        for off in offsets {
            let p = chunkStart + stringsStart + off
            guard p < chunkStart + chunkSize else { return nil }
            let s = isUTF8 ? readUTF8(data, at: p) : readUTF16(data, at: p)
            out.append(s ?? "")
        }
        return out
    }

    private static func readUTF8(_ data: Data, at p: Int) -> String? {
        guard p + 1 <= data.count else { return nil }
        var q = p
        if (Int(data[q]) & 0x80) != 0 { q += 2 } else { q += 1 }
        guard q < data.count else { return nil }
        var byteCount = Int(data[q]); q += 1
        if (byteCount & 0x80) != 0 {
            guard q < data.count else { return nil }
            byteCount = ((byteCount & 0x7F) << 8) | Int(data[q]); q += 1
        }
        guard q + byteCount <= data.count else { return nil }
        return String(data: data.subdata(in: q..<(q + byteCount)), encoding: .utf8)
    }

    private static func readUTF16(_ data: Data, at p: Int) -> String? {
        guard p + 2 <= data.count else { return nil }
        var q = p
        var charCount = Int(readUInt16(data, at: q)); q += 2
        if (charCount & 0x8000) != 0 {
            guard q + 4 <= data.count else { return nil }
            charCount = ((charCount & 0x7FFF) << 16) | Int(readUInt16(data, at: q))
            q += 4
        }
        let byteCount = charCount * 2
        guard q + byteCount <= data.count else { return nil }
        return String(data: data.subdata(in: q..<(q + byteCount)), encoding: .utf16LittleEndian)
    }

    private static func readUInt16(_ data: Data, at offset: Int) -> UInt16 {
        let lo = UInt16(data[offset])
        let hi = UInt16(data[offset + 1])
        return lo | (hi << 8)
    }

    private static func readUInt32(_ data: Data, at offset: Int) -> UInt32 {
        let b0 = UInt32(data[offset])
        let b1 = UInt32(data[offset + 1])
        let b2 = UInt32(data[offset + 2])
        let b3 = UInt32(data[offset + 3])
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)
    }
}
