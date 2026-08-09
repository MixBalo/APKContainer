//
//  DexInspector.swift
//  ApkContainer
//
//  Status: IMPLEMENTED (header inspection only; never executes DEX bytecode).
//
//  Reads the DEX header (`classes.dex` inside the APK) and returns the
//  `class_defs_size` field, which is the number of class definitions in the
//  file. This is a quick "how big is this app" signal used by the catalog UI.
//
//  DEX header layout (the fields we read):
//     0: magic (8 bytes) — "dex\n035\0" (or 036/037/038/039)
//     8: checksum (UInt32)
//    12: signature (20 bytes SHA-1)
//    32: file_size (UInt32)
//    36: header_size (UInt32) — should be 0x70
//    40: endian_tag (UInt32) — 0x12345678
//    ...
//    96: class_defs_size (UInt32)  ← offset 0x60
//   100: class_defs_off (UInt32)
//
//  Honesty contract: header only. No execution.
//

import Foundation

/// Lightweight DEX (.dex) header inspector.
public enum DexInspector {

    /// Reads `class_defs_size` from a DEX file located at `dexURL`.
    ///
    /// `zipReader` is the reader that extracted the DEX (currently unused;
    /// kept for future DEX string-table inspection that may need re-extraction
    /// from the source APK). It is intentionally part of the signature so the
    /// API can grow without breaking callers.
    public static func classCount(dexURL: URL, zipReader: ZipReader) -> Int {
        _ = zipReader // reserved for future use; documented above.
        guard let data = try? Data(contentsOf: dexURL, options: [.alwaysMapped]) else {
            return -1
        }
        return classCount(dexData: data)
    }

    /// Reads `class_defs_size` from in-memory DEX bytes.
    public static func classCount(dexData: Data) -> Int {
        guard dexData.count >= 100 else { return -1 }
        // Magic: "dex\n" then version "035"/"036"/"037"/"038"/"039" then "\0".
        let magic: [UInt8] = [0x64, 0x65, 0x78, 0x0A] // "dex\n"
        for i in 0..<4 {
            if dexData[i] != magic[i] { return -1 }
        }
        // class_defs_size lives at offset 0x60 = 96.
        let b0 = UInt32(dexData[96])
        let b1 = UInt32(dexData[97])
        let b2 = UInt32(dexData[98])
        let b3 = UInt32(dexData[99])
        let classDefsSize = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)
        return Int(classDefsSize)
    }
}
