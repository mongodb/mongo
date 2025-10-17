/**
 * Simple read-write exercise of our bson corpus.
 *
 * @tags: [
 * requires_external_data_source,
 * ]
 */

// Pack a nested object
function makeNest(depth, width, level = 0, idx = 0) {
    let obj = {};
    if (level == depth) {
        obj["foo"] = NumberInt(idx);
        obj["bar"] = idx * 1.0;
    } else {
        for (let i = 0; i < width; ++i) {
            // Numeric strings are weird in JS, and can cause key
            // reordering, so avoid them.
            obj[(i + 1).toString() + "a"] = makeNest(depth, width, level + 1, i);
        }
    }
    return obj;
}

let maxBsonUserSize = 0x1000000;
let maxBsonInternalSize = maxBsonUserSize + 0x4000;
let paddingKey = "string";
// Return a string which, when added with paddingKey, will pad
// the BSONObj to max size.
function makePadString(objLen) {
    let insertOverhead = 0x4000;
    // uint8_t, sizeof("string"), uint32_t, uint8_t (EOO)
    let internalFieldsSize = 13;
    return "x".repeat(maxBsonInternalSize - objLen - insertOverhead - internalFieldsSize - 1);
}

let tests = [];

// Case 0: Edge-case scalars
tests.push({
    _id: ObjectId(),
    // Note that INT_MAX can fit in a JS number, but
    // LLONG_MAX is too large, and must be passed through
    // the parser as a string.
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
});

// Case 1: Max BSON size handleable by server
tests.push(
    (function () {
        let ret = {_id: ObjectId()};
        ret[paddingKey] = makePadString(Object.bsonsize(ret));
        return ret;
    })(),
);

// Case 2:
// Sub object with a broad branching factor: each object contains 5.
// Sub objects which in turn branch into 5 further subobject, etc.
tests.push(
    (function () {
        let obj = {_id: ObjectId()};
        Object.assign(obj, makeNest(8, 5));
        return obj;
    })(),
);

// Case 3:
// Deeply nested object: single nested chain of subobjects up to
// maximum nesting depth allowed in js representation of bson object,
// minus levels for containing message.
let maxJSBsonDepth = 146;
tests.push(
    (function () {
        let obj = {_id: ObjectId()};
        Object.assign(obj, makeNest(maxJSBsonDepth, 1));
        return obj;
    })(),
);

// Case 4: Creates as many nested objects as can fit in max size allowance.
tests.push(
    (function () {
        let key = maxBsonInternalSize * 10; // keep string length the same
        let subObj = makeNest(6, 3);
        let ret = {_id: ObjectId()};
        let minLen = Object.bsonsize(ret);
        // avoid numeric string keys again
        ret[(key++).toString() + "a"] = subObj;
        let nestLen = Object.bsonsize(ret);
        let numPack = (maxBsonUserSize - nestLen - 1) / (nestLen - minLen) - 1;
        for (let i = 0; i < numPack; ++i) {
            ret[(key + i).toString() + "a"] = subObj;
        }
        return ret;
    })(),
);

// Case 5: Giant array
tests.push(
    (function () {
        let arr = [];
        for (let i = 0; i < 0x100000; ++i) {
            arr.push(NumberInt(i));
        }
        return {_id: ObjectId(), array: arr};
    })(),
);

// Case 6: 1024 arrays each with 5 elements.
tests.push(
    (function () {
        let arr = [];
        for (let i = 0; i < 1024; ++i) {
            arr.push([NumberInt(0), NumberInt(1), NumberInt(2), NumberInt(3), NumberInt(4)]);
        }
        return {_id: ObjectId(), array: arr};
    })(),
);

// Case 7: Exercise remaining types and fill size limit
tests.push(
    (function () {
        let data = HexData(0, "01".repeat(1024));
        let str = data.toString();
        let oid = ObjectId("01".repeat(12));
        let ret = {
            _id: ObjectId(),
            minkey: MinKey(),
            maxkey: MaxKey(),
            bindata: data,
            oid: oid,
            bool: true,
            date: Date(0),
            "null": null,
            regex: RegExp(str),
            dbref: DBRef(str, oid),
            code: Code(str),
            //        symbol:Symbol(str),
            codewscope: Code(str, {a: 1}),
        };
        ret[paddingKey] = makePadString(Object.bsonsize(ret));
        return ret;
    })(),
);

// Case 8: Add a bunch of invalid null bytes in the middle of a string
tests.push({
    _id: ObjectId(),
    String0: "\0a",
    String1: "a\0",
    String2: "\0",
    String3: "\0\0\0",
    String4: "a\0\x01\x08a",
    String5: "a\0\x02\x08b",
    String6: "a\0\x01\x10",
    String7: "a\0\x01\xc0",
    String8: "a\0\x01\0x3d\0\xff\xff\xff\xff\0\x08b",
});

// Runs tests on a standalone mongod.
let conn = MongoRunner.runMongod({setParameter: {enableComputeMode: true}});
let db = conn.getDB(jsTestName());

////////////////////////////////////////////////////////////////////////////////////////////////
// Test handling of some bson edge cases by attempting insert and fetch with content
// generated by src/mongo/bson/util:bson_corpus_gen and checking that it correctly reads back.
////////////////////////////////////////////////////////////////////////////////////////////////
(function testBSONCorpus() {
    jsTestLog("Testing testBSONCorpus()");

    // Verify the objects read from the pipes match what was written to them.
    for (let objIdx = 0; objIdx < tests.length; ++objIdx) {
        jsTestLog("Running " + objIdx);
        let obj = tests[objIdx];
        assert.commandWorked(db.test.insert(obj));
        let foundObj = db.test.findOne({_id: obj._id});
        assert.eq(foundObj, obj);
        // TODO SERVER-110471 use this stricter comparison
        // assert(bsonBinaryEqual(foundObj, obj), `[${tojson(foundObj)}] and [${tojson(obj)}] are not equal`, {foundObj, obj})
    }
})();

MongoRunner.stopMongod(conn);
