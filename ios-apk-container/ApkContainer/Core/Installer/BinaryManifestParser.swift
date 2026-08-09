//
//  BinaryManifestParser.swift
//  ApkContainer
//
//  Status: IMPLEMENTED (real, from-scratch Android binary XML / AXML parser).
//
//  Parses the Android binary XML format (the form `AndroidManifest.xml` and
//  other layout/menu XMLs take inside an APK). Reads:
//    - File header (magic 0x00080003 = type 0x0003 + header size 0x0008).
//    - RES_STRING_POOL_TYPE (0x001C is the header size; chunk type 0x0001).
//    - RES_XML_RESOURCE_MAP_TYPE (0x0180).
//    - RES_XML_START_NAMESPACE_TYPE (0x0100) / END (0x0101).
//    - RES_XML_START_ELEMENT_TYPE (0x0102) / END (0x0103).
//    - RES_XML_CDATA_TYPE (0x0104).
//
//  Resolves attribute resource IDs for the well-known names listed below using
//  the resource map chunk (string index → resource ID, then reverse-map to a
//  canonical attribute name).
//
//  Limitation (called out in comments): does NOT resolve @string/@drawable
//  references — that requires resources.arsc + a full value table. Only literal
//  string values and known resource-ID-tagged attribute names are resolved.
//  Reference-valued attributes (TYPE_REFERENCE) are emitted as "@res0x01010001"
//  strings so the caller can recognize them.
//
//  Honesty contract: real AXML walk; no execution of any bytecode.
//

import Foundation

/// Errors raised by `BinaryManifestParser`.
public enum BinaryManifestError: LocalizedError {
    case truncated
    case badMagic
    case unknownChunkType(UInt16)
    case malformedStringPool(String)
    case malformedAttribute

    public var errorDescription: String? {
        switch self {
        case .truncated:                          return "AXML truncated."
        case .badMagic:                           return "AXML magic 0x00080003 not found."
        case .unknownChunkType(let t):            return "Unknown AXML chunk type 0x\(String(t, radix: 16))."
        case .malformedStringPool(let why):       return "Malformed string pool: \(why)."
        case .malformedAttribute:                 return "Malformed attribute."
        }
    }
}

/// Android binary XML (AXML) parser.
///
/// Output schema: a nested `[String: Any]` mirroring the XML tree. Each element
/// node is itself a `[String: Any]` with:
///   - `"_attrs"`: `[String: String]` — attribute name → string value
///   - `"_tag"`:   `String` — the element's tag name
///   - `"_children"`: `[[String: Any]]` — child element nodes, in document order
///   - For each child tag name, a parallel convenience key maps to either the
///     single child node (if there is exactly one) or an array of nodes (if
///     there are multiple). This mirrors how XML parsers like ElementTree expose
///     repeated children, and is what `ApkParser` relies on.
public enum BinaryManifestParser {

    public static func parse(_ data: Data) throws -> [String: Any] {
        let p = _Parser(data: data)
        return try p.parse()
    }
}

// MARK: - Known Android resource IDs for attribute names

/// Reverse map: Android resource ID → canonical attribute name. Used to recover
/// attribute names when AAPT has stripped them from the string pool (the
/// resource map still carries the ID).
private let kAttrResIDToName: [UInt32: String] = [
    0x01010000: "theme",
    0x01010001: "label",
    0x01010002: "icon",
    0x01010003: "name",
    0x01010004: "style",
    0x01010005: "class",
    0x0101000F: "debuggable",
    0x0101000C: "permission",
    0x0101021A: "versionCode",
    0x0101021B: "versionName",
    0x0101020C: "minSdkVersion",
    0x01010270: "targetSdkVersion",
    0x01010218: "sharedUserId",
    0x01010219: "sharedUserLabel",
    0x0101030D: "hasCode",
    0x01010307: "allowBackup",
    0x0101038C: "networkSecurityConfig",
    0x0101021C: "sharedUserId",
]

// MARK: - Internal parser

private final class _Parser {

    private let data: Data
    private var strings: [String] = []
    private var resourceIDs: [UInt32] = []

    // Element stack. Each entry: (tag, attrs, children-in-order).
    private var stack: [(tag: String, attrs: [String: String], children: [[String: Any]])] = []

    init(data: Data) {
        self.data = data
    }

    func parse() throws -> [String: Any] {
        guard data.count >= 8 else { throw BinaryManifestError.truncated }
        // File header: type (UInt16) | headerSize (UInt16) | chunkSize (UInt32)
        let type = readUInt16(at: 0)
        let headerSize = readUInt16(at: 2)
        guard type == 0x0003 && headerSize == 0x0008 else {
            throw BinaryManifestError.badMagic
        }
        let totalSize = Int(readUInt32(at: 4))
        guard totalSize <= data.count else { throw BinaryManifestError.truncated }

        // Walk chunks. Position right after the 8-byte file header.
        var offset = 8
        // The result root is a synthetic wrapper so callers can find the root
        // element by tag name.
        var root: [String: Any] = ["_attrs": [:], "_tag": "", "_children": []]

        while offset + 8 <= totalSize {
            let chunkType = readUInt16(at: offset)
            let chunkHeaderSize = Int(readUInt16(at: offset + 2))
            let chunkSize = Int(readUInt32(at: offset + 4))
            guard chunkSize > 0, offset + chunkSize <= totalSize else {
                throw BinaryManifestError.truncated
            }
            switch chunkType {
            case 0x0001: // RES_STRING_POOL_TYPE
                try parseStringPool(chunkStart: offset, chunkSize: chunkSize)
            case 0x0180: // RES_XML_RESOURCE_MAP_TYPE
                parseResourceMap(chunkStart: offset, chunkSize: chunkSize)
            case 0x0100: // RES_XML_START_NAMESPACE_TYPE
                // No-op: we don't need namespace bindings to resolve names.
                break
            case 0x0101: // RES_XML_END_NAMESPACE_TYPE
                break
            case 0x0102: // RES_XML_START_ELEMENT_TYPE
                let node = try parseStartElement(at: offset, headerSize: chunkHeaderSize)
                pushElement(node)
            case 0x0103: // RES_XML_END_ELEMENT_TYPE
                let popped = try popEndElement(at: offset)
                if stack.isEmpty {
                    // Popped the root element.
                    root = popped
                }
            case 0x0104: // RES_XML_CDATA_TYPE
                // Ignored: manifests don't carry meaningful CDATA.
                break
            default:
                // Unknown chunk types are skipped, not fatal — forward-compat
                // with future AAPT versions.
                break
            }
            offset += chunkSize
        }

        return root
    }

    // MARK: - String pool

    /// RES_STRING_POOL_TYPE chunk layout (offsets from chunkStart):
    ///   0: chunk type (UInt16) = 0x0001
    ///   2: chunk header size (UInt16) = 0x001C
    ///   4: chunk size (UInt32)
    ///   8: string count (UInt32)
    ///  12: style count (UInt32)
    ///  16: flags (UInt32)  — bit 1<<8 = UTF-8 encoded
    ///  20: strings start (UInt32) — offset from chunkStart
    ///  24: styles start (UInt32) — offset from chunkStart
    ///  28: string offsets (stringCount * 4 bytes)
    ///  28 + stringCount*4: style offsets (styleCount * 4 bytes)
    ///  stringsStart: string data
    private func parseStringPool(chunkStart: Int, chunkSize: Int) throws {
        guard chunkStart + 28 <= data.count else {
            throw BinaryManifestError.malformedStringPool("header truncated")
        }
        let stringCount = Int(readUInt32(at: chunkStart + 8))
        let flags = readUInt32(at: chunkStart + 16)
        let isUTF8 = (flags & (1 << 8)) != 0
        let stringsStart = Int(readUInt32(at: chunkStart + 20))

        var offsets: [Int] = []
        offsets.reserveCapacity(stringCount)
        for i in 0..<stringCount {
            let fieldOff = chunkStart + 28 + i * 4
            guard fieldOff + 4 <= data.count else {
                throw BinaryManifestError.malformedStringPool("offset table truncated")
            }
            offsets.append(Int(readUInt32(at: fieldOff)))
        }

        strings = []
        strings.reserveCapacity(stringCount)
        for off in offsets {
            let p = chunkStart + stringsStart + off
            guard p < chunkStart + chunkSize else {
                throw BinaryManifestError.malformedStringPool("string offset out of bounds")
            }
            do {
                let s = isUTF8 ? try readUTF8String(at: p) : try readUTF16String(at: p)
                strings.append(s)
            } catch {
                strings.append("")
            }
        }
    }

    private func readUTF8String(at p: Int) throws -> String {
        guard p + 1 <= data.count else { throw BinaryManifestError.truncated }
        // First 1-2 bytes: UTF-16 char count (we ignore this; we use the byte count).
        var q = p
        var charCountHi = Int(data[q]); q += 1
        if (charCountHi & 0x80) != 0 {
            _ = Int(data[q]); q += 1 // low byte of char count — unused
        }
        // Next 1-2 bytes: UTF-8 byte count.
        guard q < data.count else { throw BinaryManifestError.truncated }
        var byteCount = Int(data[q]); q += 1
        if (byteCount & 0x80) != 0 {
            guard q < data.count else { throw BinaryManifestError.truncated }
            byteCount = ((byteCount & 0x7F) << 8) | Int(data[q]); q += 1
        }
        guard q + byteCount <= data.count else {
            throw BinaryManifestError.malformedStringPool("UTF-8 string runs past EOF")
        }
        let bytes = data.subdata(in: q..<(q + byteCount))
        return String(data: bytes, encoding: .utf8) ?? ""
    }

    private func readUTF16String(at p: Int) throws -> String {
        guard p + 2 <= data.count else { throw BinaryManifestError.truncated }
        var q = p
        var charCount = Int(readUInt16(at: q)); q += 2
        if (charCount & 0x8000) != 0 {
            // High bit set: actual length is in the following UInt32.
            guard q + 4 <= data.count else { throw BinaryManifestError.truncated }
            charCount = ((charCount & 0x7FFF) << 16) | Int(readUInt16(at: q))
            q += 4 // skip the rest of the 32-bit length (we read low 16 only)
        }
        let byteCount = charCount * 2
        guard q + byteCount <= data.count else {
            throw BinaryManifestError.malformedStringPool("UTF-16 string runs past EOF")
        }
        let bytes = data.subdata(in: q..<(q + byteCount))
        return String(data: bytes, encoding: .utf16LittleEndian) ?? ""
    }

    // MARK: - Resource map

    /// RES_XML_RESOURCE_MAP_TYPE (0x0180): a flat UInt32 array, one entry per
    /// string-pool index, giving that string's Android resource ID (or 0).
    private func parseResourceMap(chunkStart: Int, chunkSize: Int) {
        let arrayStart = chunkStart + 8
        let arrayBytes = chunkSize - 8
        let count = arrayBytes / 4
        resourceIDs = []
        resourceIDs.reserveCapacity(count)
        for i in 0..<count {
            let p = arrayStart + i * 4
            guard p + 4 <= data.count else { break }
            resourceIDs.append(readUInt32(at: p))
        }
    }

    // MARK: - Elements

    /// RES_XML_START_ELEMENT_TYPE (0x0102) chunk layout (offsets from `at`):
    ///   0: chunk type (UInt16)
    ///   2: chunk header size (UInt16) = 0x0010 (16)
    ///   4: chunk size (UInt32)
    ///   8: line number (UInt32)
    ///  12: comment (UInt32) — string index, or 0xFFFFFFFF
    ///  16: namespace (UInt32) — string index, or 0xFFFFFFFF
    ///  20: name (UInt32) — string index
    ///  24: attribute start (UInt16) — usually 0x0014
    ///  26: attribute size (UInt16) — usually 0x0014 (20)
    ///  28: attribute count (UInt16)
    ///  30: id index (UInt16)
    ///  32: class index (UInt16)
    ///  34: style index (UInt16)
    ///  36: attributes (attrCount * attrSize bytes)
    private func parseStartElement(at offset: Int, headerSize: Int) throws -> [String: Any] {
        guard offset + 36 <= data.count else { throw BinaryManifestError.truncated }
        let nameIdx = readUInt32(at: offset + 20)
        let attrSize = Int(readUInt16(at: offset + 26))
        let attrCount = Int(readUInt16(at: offset + 28))
        let tag = string(at: Int(nameIdx)) ?? ""

        var attrs: [String: String] = [:]
        // Attributes begin at `offset + 36`. The standard AXML layout has:
        //   ResXMLTree_node header (16 bytes: type, headerSize, size, lineNumber, comment)
        //   ResXMLTree_attrExt (20 bytes: ns, name, attributeStart, attributeSize,
        //                       attributeCount, idIndex, classIndex, styleIndex)
        //   ... then attributes.
        // `attributeStart` (at offset+24) is always 0x14 (20), pointing from
        // the start of ResXMLTree_attrExt (offset+16) to where attributes
        // begin: 16 + 20 = 36. We use the absolute offset directly.
        let attrsAbsolute = offset + 36
        _ = headerSize // headerSize is the size of ResXMLTree_node (16); unused here.

        for i in 0..<attrCount {
            let a = attrsAbsolute + i * attrSize
            guard a + 20 <= data.count else { throw BinaryManifestError.malformedAttribute }
            // Attribute (20 bytes):
            //   0: namespace (UInt32)
            //   4: name (UInt32)  — string index
            //   8: raw value (UInt32) — string index, or 0xFFFFFFFF
            //  12: typed value size (UInt16) — should be 0x0008
            //  14: res0 (UInt8) — 0
            //  15: type (UInt8)
            //  16: data (UInt32)
            let attrNameIdx = readUInt32(at: a + 4)
            let rawIdx = readUInt32(at: a + 8)
            let type = data[a + 15]
            let valueData = readUInt32(at: a + 16)

            let attrName = resolveAttrName(stringIndex: Int(attrNameIdx))
            let value = attrValue(
                type: type,
                rawIdx: Int(rawIdx),
                data: valueData
            )
            if let n = attrName, let v = value {
                attrs[n] = v
            }
        }

        return [
            "_tag": tag,
            "_attrs": attrs,
            "_children": [[String: Any]](),
        ]
    }

    /// Resolve attribute name. AAPT sometimes leaves the string-pool slot empty
    /// and relies on the resource map's ID; in that case we reverse-map via
    /// `kAttrResIDToName`.
    private func resolveAttrName(stringIndex: Int) -> String? {
        if stringIndex >= 0 && stringIndex < strings.count {
            let s = strings[stringIndex]
            if !s.isEmpty { return s }
        }
        // Fall back to the resource map.
        if stringIndex >= 0 && stringIndex < resourceIDs.count {
            let resID = resourceIDs[stringIndex]
            if let name = kAttrResIDToName[resID] { return name }
        }
        return nil
    }

    /// Convert an attribute's typed value to a Swift String.
    ///
    /// LIMITATION: TYPE_REFERENCE values are emitted as `"@res0x01010001"`
    /// placeholder strings — we do NOT resolve @string/@drawable references
    /// because that needs resources.arsc + a full value table. Callers that
    /// care (label/icon resolution) must run `ResourcesArscReader` afterwards.
    private func attrValue(type: UInt8, rawIdx: Int, data valueData: UInt32) -> String? {
        switch type {
        case 0x00: // TYPE_NULL
            return ""
        case 0x03: // TYPE_STRING
            return string(at: rawIdx)
        case 0x10: // TYPE_INT_DEC
            return String(Int32(bitPattern: valueData))
        case 0x11: // TYPE_INT_HEX
            return "0x\(String(valueData, radix: 16))"
        case 0x12: // TYPE_INT_BOOLEAN
            return valueData != 0 ? "true" : "false"
        case 0x01: // TYPE_REFERENCE
            return "@res0x\(String(format: "%08x", valueData))"
        case 0x02: // TYPE_ATTRIBUTE
            return "?res0x\(String(format: "%08x", valueData))"
        case 0x04: // TYPE_FLOAT
            return String(Float(bitPattern: valueData))
        default:
            // DIMENSION, FRACTION, etc. — best-effort raw decimal.
            return String(valueData)
        }
    }

    private func string(at idx: Int) -> String? {
        guard idx >= 0 && idx < strings.count else { return nil }
        return strings[idx]
    }

    // MARK: - Element stack

    private func pushElement(_ node: [String: Any]) {
        let tag = (node["_tag"] as? String) ?? ""
        let attrs = (node["_attrs"] as? [String: String]) ?? [:]
        stack.append((tag: tag, attrs: attrs, children: []))
    }

    private func popEndElement(at offset: Int) throws -> [String: Any] {
        guard !stack.isEmpty else {
            throw BinaryManifestError.truncated
        }
        let entry = stack.removeLast()
        var node: [String: Any] = [
            "_tag": entry.tag,
            "_attrs": entry.attrs,
            "_children": entry.children,
        ]
        // Build convenience per-tag-name child slots.
        var byName: [String: Any] = [:]
        for child in entry.children {
            guard let childTag = child["_tag"] as? String else { continue }
            if let existing = byName[childTag] {
                if var arr = existing as? [[String: Any]] {
                    arr.append(child)
                    byName[childTag] = arr
                } else if let single = existing as? [String: Any] {
                    byName[childTag] = [single, child]
                }
            } else {
                byName[childTag] = child
            }
        }
        for (k, v) in byName { node[k] = v }

        // Attach to parent (if any) — or return as root.
        if !stack.isEmpty {
            stack[stack.count - 1].children.append(node)
        }
        return node
    }

    // MARK: - LE readers

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
}
