/**
 * Utilities for laying out raw BSON documents by hand in a jstest and writing them to a file as
 * bytes. These exist for documents a normal BSON builder cannot produce, such as ones larger than
 * BSONObjMaxUserSize; prefer BSON() for anything a builder can express.
 */

// writeFile() UTF-8 encodes any byte at or above this, so every byte written must stay below it.
const kUtf8EncodeThreshold = 0x80;
const kMaxLowByte = kUtf8EncodeThreshold - 1;

/**
 * The largest string field length whose four little-endian bytes all stay below the UTF-8 encoding
 * threshold, and so the longest single field a document built here can carry.
 */
export const kMaxLowByteStrLen = (kMaxLowByte << 16) | (kMaxLowByte << 8) | kMaxLowByte;

/**
 * Returns 'n' as a 4-byte little-endian string, asserting that it survives the trip to a file
 * unchanged.
 */
export function int32LE(n) {
    const bytes = [n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, (n >>> 24) & 0xff];
    for (const b of bytes) {
        assert.lt(
            b,
            kUtf8EncodeThreshold,
            "int32 contains a byte that writeFile() would UTF-8 encode and corrupt",
            {value: n, byte: b},
        );
    }
    return String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]);
}

/**
 * Builds a well-formed BSON document holding one string field per entry of 'strLens', returning its
 * raw bytes as a string. The layout is:
 *   int32 totalSize | (0x02 | key | 0x00 | int32 strLen | strLen bytes)... | 0x00
 * Each 'strLen' includes the string's own NUL terminator, so a document's total size is
 * (5 + sum over fields of (7 + strLen)). Fields are keyed "a", "b", ... in order.
 */
export function makeRawStringBson(strLens) {
    let body = "";
    for (let i = 0; i < strLens.length; ++i) {
        const strLen = strLens[i];
        assert.gt(strLen, 0, "string length must leave room for its NUL terminator", {strLen});
        body +=
            "\x02" + // type: string
            String.fromCharCode("a".charCodeAt(0) + i) + // key: "a", "b", ... (single char)
            "\x00" +
            int32LE(strLen) +
            "x".repeat(strLen - 1) +
            "\x00"; // string contents and its terminator
    }
    return int32LE(4 + body.length + 1) + body + "\x00";
}

/**
 * Writes 'contents' to 'path' as raw bytes, 'chunkSize' at a time. Arguments to the shell's file
 * builtins are marshalled through a BSON object, so 'chunkSize' must stay well under the BSON size
 * limit that the server reports as hello().maxBsonObjectSize.
 */
export function writeBinaryFileInChunks(path, contents, chunkSize) {
    writeFile(path, contents.substring(0, chunkSize), true /* useBinaryMode */);
    for (let offset = chunkSize; offset < contents.length; offset += chunkSize) {
        appendFile(path, contents.substring(offset, offset + chunkSize), true /* useBinaryMode */);
    }
}
