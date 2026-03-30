/**
 * Combinatorial golden test for $bitsAllSet, $bitsAllClear, $bitsAnySet, $bitsAnyClear.
 *
 * A diverse document collection (integers and BinData of various widths) is queried
 * with every combination of operator × expression type (integer mask, array of positions,
 * BinData mask). The golden output captures current semantics as the reference.
 */

import {show} from "jstests/libs/golden_test.js";

const coll = db.query_golden_bittest;
coll.drop();

// Documents
// Each doc has a single field `val`. Documents are ordered by _id so the
// show() output (which sorts by JSON value) has a predictable shape.
assert.commandWorked(
    coll.insert([
        // --- integers: 1-byte patterns (bits 0-7) ---
        {_id: 1, val: NumberInt(0)}, // no bits set
        {_id: 2, val: NumberInt(1)}, // bit 0 only
        {_id: 3, val: NumberInt(128)}, // bit 7 only
        {_id: 4, val: NumberInt(54)}, // 0x36: bits 1,2,4,5
        {_id: 5, val: NumberInt(88)}, // 0x58: bits 3,4,6
        {_id: 6, val: NumberInt(255)}, // 0xFF: bits 0-7

        // --- integers: 2-byte patterns (bits 8-15) ---
        {_id: 7, val: NumberLong("256")}, // bit 8 only
        {_id: 8, val: NumberLong("32768")}, // bit 15 only
        {_id: 9, val: NumberLong("13824")}, // 0x3600: bits 9,10,12,13
        {_id: 10, val: NumberLong("65280")}, // 0xFF00: bits 8-15

        // --- integers: higher bits ---
        {_id: 11, val: NumberLong("65536")}, // bit 16
        {_id: 12, val: NumberLong("4294967296")}, // bit 32
        {_id: 13, val: NumberLong("72057594037927936")}, // bit 56
        {_id: 14, val: NumberLong("9223372036854775807")}, // bits 0-62 (INT64_MAX)

        // --- BinData: 1-byte patterns ---
        {_id: 15, val: BinData(0, "AA==")}, // 0x00: no bits
        {_id: 16, val: BinData(0, "AQ==")}, // 0x01: bit 0
        {_id: 17, val: BinData(0, "gA==")}, // 0x80: bit 7
        {_id: 18, val: BinData(0, "Ng==")}, // 0x36: bits 1,2,4,5
        {_id: 19, val: BinData(0, "WA==")}, // 0x58: bits 3,4,6
        {_id: 20, val: BinData(0, "/w==")}, // 0xFF: bits 0-7

        // --- BinData: 2-byte patterns (bits 8-15) ---
        {_id: 21, val: BinData(0, "AAE=")}, // 0x0001: bit 8
        {_id: 22, val: BinData(0, "AIA=")}, // 0x0080: bit 15
        {_id: 23, val: BinData(0, "ADY=")}, // 0x0036: bits 9,10,12,13
        {_id: 24, val: BinData(0, "AP8=")}, // 0x00FF: bits 8-15
        {_id: 25, val: BinData(0, "NjY=")}, // 0x3636: bits 1,2,4,5,9,10,12,13
        {_id: 26, val: BinData(0, "//8=")}, // 0xFFFF: bits 0-15

        // --- BinData: wider patterns ---
        {_id: 27, val: BinData(0, "AAAB")}, // 3 bytes: bit 16
        {_id: 28, val: BinData(0, "AACA")}, // 3 bytes: bit 23
        {_id: 29, val: BinData(0, "AAAAAAE=")}, // 5 bytes: bit 32

        // --- BinData: 9 bytes — bits 64-71 are beyond int64 ---
        {_id: 30, val: BinData(0, "AAAAAAAAAAAB")}, // bit 64 (byte 8 LSB)
        {_id: 31, val: BinData(0, "AAAAAAAAAACA")}, // bit 71 (byte 8 MSB)
        {_id: 32, val: BinData(0, "AQEBAQEBAQEB")}, // bits 0,8,16,24,32,40,48,56,64
        {_id: 33, val: BinData(0, "////////////")}, // bits 0-71 all set

        // --- negative integers (sign-extended: all bit positions >= 64 are set) ---
        {_id: 34, val: NumberLong(-1)}, // all 64 bits set; sign-extended → every position set
        {_id: 35, val: NumberLong(-5)}, // 0xFFFFFFFFFFFFFFFB: all bits set except bit 2
    ]),
);

// ---------------------------------------------------------------------------
// Combinatorial queries
// ---------------------------------------------------------------------------
const operators = ["$bitsAllSet", "$bitsAllClear", "$bitsAnySet", "$bitsAnyClear"];

// Integer bitmasks (including negatives, which are sign-extended in two's complement)
const intMasks = [0, 1, 54, 128, 255, 13824, 65280, -1, -5];

// Arrays of bit positions — spans 1-byte, 2-byte, and beyond-int64 ranges
const arrays = [
    [],
    [0],
    [7],
    [1, 2, 4, 5],
    [3, 4, 6],
    [0, 1, 2, 3, 4, 5, 6, 7],
    [8],
    [15],
    [9, 10, 12, 13],
    [8, 9, 10, 11, 12, 13, 14, 15],
    [16],
    [32],
    [56],
    [64],
    [71],
    [0, 8, 16, 24, 32, 40, 48, 56, 64],
    [63], // sign bit of int64 — set in all negative integers
    [200], // far beyond int64 — set only in sign-extended negatives, not in any BinData doc
];

// BinData bitmasks — 1-byte through 9-byte
const binMasks = [
    BinData(0, "AA=="), // 0x00 (1 byte)
    BinData(0, "AQ=="), // bit 0
    BinData(0, "gA=="), // bit 7
    BinData(0, "Ng=="), // bits 1,2,4,5 (1 byte)
    BinData(0, "/w=="), // 0xFF (1 byte)
    BinData(0, "AAE="), // bit 8 (2 bytes)
    BinData(0, "AIA="), // bit 15 (2 bytes)
    BinData(0, "ADY="), // bits 9,10,12,13 (2 bytes)
    BinData(0, "AP8="), // bits 8-15 (2 bytes)
    BinData(0, "//8="), // bits 0-15 (2 bytes)
    BinData(0, "AAAAAAE="), // bit 32 (5 bytes)
    BinData(0, "AAAAAAAAAAAB"), // bit 64 (9 bytes)
    BinData(0, "////////////"), // bits 0-71 (9 bytes)
];

for (const op of operators) {
    jsTestLog(`=== ${op} ===`);

    for (const mask of intMasks) {
        jsTestLog(`${op} int-mask ${mask}`);
        try {
            show(
                coll
                    .find({val: {[op]: mask}}, {_id: 0})
                    .sort({_id: 1})
                    .toArray(),
            );
        } catch (e) {
            print(`${e.code}`);
        }
    }

    for (const arr of arrays) {
        jsTestLog(`${op} array ${tojson(arr)}`);
        try {
            show(
                coll
                    .find({val: {[op]: arr}}, {_id: 0})
                    .sort({_id: 1})
                    .toArray(),
            );
        } catch (e) {
            print(`${e.code}`);
        }
    }

    for (const mask of binMasks) {
        jsTestLog(`${op} BinData-mask ${tojson(mask)}`);
        try {
            show(
                coll
                    .find({val: {[op]: mask}}, {_id: 0})
                    .sort({_id: 1})
                    .toArray(),
            );
        } catch (e) {
            print(`${e.code}`);
        }
    }
}
