/**
 * A corpus of BSON documents exercising all supported BSON types, sizes and structure.
 */

/**
 * BSON types and BinData subtypes the corpus deliberately leaves out, each with the reason.
 */
export const kBsonCorpusExclusions = [
    {
        type: "symbol",
        reason: "Deprecated. The shell reads symbols as strings and has no way to write one.",
    },
];

// Pack a nested BSON object `depth` levels deep with `width` BSON objects per level.
function makeNest(depth, width, level = 0, idx = 0) {
    let obj = {};
    if (level == depth) {
        obj["foo"] = NumberInt(idx);
        obj["bar"] = idx * 1.0;
    } else {
        for (let i = 0; i < width; ++i) {
            obj[(i + 1).toString() + "a"] = makeNest(depth, width, level + 1, i);
        }
    }
    return obj;
}

// Appends a string field to `doc` so that its BSON size is exactly `targetSize`.
function padToSize(doc, targetSize, fieldName = "string") {
    // type byte + field name + NUL + int32 length + string NUL
    const fieldOverhead = 1 + fieldName.length + 1 + 4 + 1;
    doc[fieldName] = "x".repeat(targetSize - Object.bsonsize(doc) - fieldOverhead);
    return doc;
}

// Each entry's build(_id, limits) returns the document, with _id as its first field.
const kEntries = [
    {
        name: "scalar_limits",
        description: "int, long, double and Decimal128 limits, zeros, NaNs and infinities",
        build: (_id) => ({
            _id,
            // INT_MAX fits in a JS number, but LLONG_MAX does not and must be passed as a string.
            "max int": NumberInt(0x7fffffff),
            "max long": NumberLong("9223372036854775807"),
            "max double": Number.MAX_VALUE,
            "zero int": NumberInt(0),
            "zero long": NumberLong(0),
            "zero double": 0.0,
            "min int": NumberInt(-0x80000000),
            "min long": NumberLong("-9223372036854775808"),
            "min double": Number.MIN_VALUE,
            "max 128-bit positive": _decimal128Limit("+Max"),
            "min 128-bit positive": _decimal128Limit("+Min"),
            "max 128-bit negative": _decimal128Limit("-Max"),
            "min 128-bit negative": _decimal128Limit("-Min"),
            "zero 128-bit": NumberDecimal(0),
            "NaN positive 128-bit": NumberDecimal("+NaN"),
            "NaN negative 128-bit": NumberDecimal("-NaN"),
            "inf positive 128-bit": NumberDecimal("+Infinity"),
            "inf negative 128-bit": NumberDecimal("-Infinity"),
        }),
    },
    {
        name: "max_size_string",
        description: "A single string padding the document to one byte under maxBsonObjectSize",
        build: (_id, {maxBsonObjectSize}) => padToSize({_id}, maxBsonObjectSize - 1),
    },
    {
        name: "wide_tree",
        description: "Subobjects branching 5 ways at each of 8 levels",
        build: (_id) => Object.assign({_id}, makeNest(8, 5)),
    },
    {
        name: "deep_nesting",
        description: "A single chain of subobjects at the deepest nesting the shell can encode",
        // The shell's limit, minus levels for the containing message.
        build: (_id) => Object.assign({_id}, makeNest(146, 1)),
    },
    {
        name: "packed_subobjects",
        description: "As many copies of a nested subobject as fit under maxBsonObjectSize",
        build: (_id, {maxBsonObjectSize}) => {
            // Nine-digit keys, so that every key has the same length.
            let key = 100000000;
            let subObj = makeNest(6, 3);
            let ret = {_id};
            let minLen = Object.bsonsize(ret);
            ret[(key++).toString() + "a"] = subObj;
            let nestLen = Object.bsonsize(ret);
            let numPack = (maxBsonObjectSize - nestLen - 1) / (nestLen - minLen) - 1;
            for (let i = 0; i < numPack; ++i) {
                ret[(key + i).toString() + "a"] = subObj;
            }
            return ret;
        },
    },
    {
        name: "giant_array",
        description: "An array of 2^20 int32 elements",
        build: (_id) => {
            let arr = [];
            for (let i = 0; i < 0x100000; ++i) {
                arr.push(NumberInt(i));
            }
            return {_id, array: arr};
        },
    },
    {
        name: "many_small_arrays",
        description: "1024 arrays of 5 elements each",
        build: (_id) => {
            let arr = [];
            for (let i = 0; i < 1024; ++i) {
                arr.push([NumberInt(0), NumberInt(1), NumberInt(2), NumberInt(3), NumberInt(4)]);
            }
            return {_id, array: arr};
        },
    },
    {
        name: "misc_types",
        description: "MinKey, MaxKey, BinData, ObjectId, bool, date, null, regex, DBRef and code",
        build: (_id) => {
            let data = HexData(0, "01".repeat(1024));
            let str = data.toString();
            let oid = ObjectId("01".repeat(12));
            return {
                _id,
                minkey: MinKey(),
                maxkey: MaxKey(),
                bindata: data,
                oid: oid,
                bool: true,
                date: new Date(0),
                "null": null,
                regex: RegExp(str),
                dbref: DBRef(str, oid),
                code: Code(str),
                codewscope: Code(str, {a: 1}),
            };
        },
    },
    {
        name: "embedded_nul_strings",
        description: "Strings with NUL bytes at the start, middle and end",
        build: (_id) => ({
            _id,
            String0: "\0a",
            String1: "a\0",
            String2: "\0",
            String3: "\0\0\0",
            String4: "a\0\x01\x08a",
            String5: "a\0\x02\x08b",
            String6: "a\0\x01\x10",
            String7: "a\0\x01\xc0",
            String8: "a\0\x01\x3d\0\xff\xff\xff\xff\0\x08b",
        }),
    },
    {
        name: "strings_utf8",
        description: "Multi-byte and combining UTF-8 in values, and a non-ASCII field name",
        build: (_id) => ({
            _id,
            str: "string",
            utf8: "élan ünïcødé 日本語 한국어 ź͖",
            "clé_ключ_键": "non-ascii field name",
        }),
    },
    {
        name: "double_specials",
        description: "Negative zero, NaN and the infinities as doubles",
        build: (_id) => ({
            _id,
            plain: 3.14159,
            negative: -273.15,
            negZero: -0,
            nan: NaN,
            inf: Infinity,
            negInf: -Infinity,
        }),
    },
    {
        name: "int64_beyond_double",
        description: "An int64 that a double cannot represent exactly",
        build: (_id) => ({_id, value: NumberLong("9007199254740993")}), // 2^53 + 1
    },
    {
        name: "decimal128_encodings",
        description: "Decimal128 negative zero, an ordinary value, and equal values encoded apart",
        build: (_id) => ({
            _id,
            plain: NumberDecimal("9823.1297"),
            negZero: NumberDecimal("-0"),
            // Numerically equal but stored with different exponents, so their bytes differ.
            oneTenths: NumberDecimal("1.0"),
            oneHundredths: NumberDecimal("1.00"),
            zeroExp10: NumberDecimal("0E+10"),
        }),
    },
    {
        name: "dates_and_timestamps",
        description: "Dates around the epoch and far from it, and timestamps",
        build: (_id) => ({
            _id,
            date: new Date("2020-05-18T14:10:30Z"),
            preEpoch: new Date("1901-01-01T00:00:00Z"), // negative millis
            farFuture: new Date("9999-12-31T23:59:59Z"),
            ts: Timestamp(1590000000, 7),
            // Stored as written. Only a top-level Timestamp(0, 0) is replaced on insert.
            nested: {zeroTs: Timestamp(0, 0)},
        }),
    },
    {
        name: "binary_subtypes",
        description: "BinData subtypes other than 0, including the deprecated 2 and 3",
        build: (_id) => ({
            _id,
            func: BinData(1, "Zm4oKXsxfQ=="),
            oldBinary: BinData(2, "BAAAAAECAwQ="), // int32 length prefix, then 4 bytes
            oldUuid: BinData(3, "Mm2Sry12RSugP2nwWrmEFg=="),
            uuid: UUID("326d92af-2d76-452b-a03f-69f05ab98416"),
            md5: MD5("4d3696564b1f6d3ae8f57771b57b8f3e"),
            encrypted: BinData(6, "AjJtkq8tdkUroD9p8Fq5hBYCY2lwaGVydGV4dA=="),
            column: BinData(7, "CQDoAwAAAAAAAAA="),
            sensitive: BinData(8, "c2Vuc2l0aXZl"),
            vector: BinData(9, "AwABAgM="), // int8 vector [1, 2, 3]
            userDefined: BinData(128, "dXNlciBkYXRh"),
        }),
    },
    {
        name: "special_scalars",
        description: "false, a regex with options, and the deprecated undefined and DBPointer",
        build: (_id) => ({
            _id,
            boolFalse: false,
            regexWithOptions: /ab+c/i,
            undef: undefined,
            dbPointer: DBPointer("db.coll", ObjectId("02".repeat(12))),
        }),
    },
    {
        name: "containers",
        description: "Mixed-type arrays, arrays of documents, empty containers, nested arrays",
        build: (_id) => {
            let deepArr = [NumberInt(1)];
            for (let i = 0; i < 10; i++) {
                deepArr = [deepArr];
            }
            return {
                _id,
                mixedArr: [1, "two", [3], {four: 4}],
                docArr: [{a: 1}, {a: 2}, {a: 3}],
                emptyObj: {},
                emptyArr: [],
                deepArr: deepArr,
            };
        },
    },
    {
        // Pairs with field_missing: the two differ only in whether `maybe` exists.
        name: "explicit_null",
        description: "A field explicitly set to null",
        build: (_id) => ({_id, maybe: null}),
    },
    {
        name: "field_missing",
        description: "The same document as explicit_null without the field",
        build: (_id) => ({_id}),
    },
    {
        name: "multi_page",
        description: "A 1MB string, so the document spans several storage pages",
        build: (_id) => ({_id, bigStr: "x".repeat(1024 * 1024)}),
    },
];

/**
 * An entry of the corpus.
 *
 * @typedef {Object} BsonCorpusEntry
 * @property {string} name Stable, unique identifier. Also the document's _id.
 * @property {string} description The edge case the document exercises.
 * @property {Object} doc The document.
 */

/**
 * Builds the corpus.
 *
 * @param {Object} [options]
 * @param {number} [options.maxBsonObjectSize] The server's maxBsonObjectSize from hello. Documents
 *     that fill the size limit are sized against it.
 * @returns {BsonCorpusEntry[]}
 */
export function getBsonCorpus({maxBsonObjectSize = 16 * 1024 * 1024} = {}) {
    return kEntries.map(({build, ...metadata}) => ({
        ...metadata,
        doc: build(metadata.name, {maxBsonObjectSize}),
    }));
}
