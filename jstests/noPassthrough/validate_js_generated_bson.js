/**
 * Tests that BSON produced by the JavaScript engine ($function) is validated before being used by
 * the server. This prevents malformed BSON from being written to collections or flowing through the
 * aggregation pipeline.
 *
 * @tags: [requires_scripting, requires_fcv_80]
 */
(function() {
"use strict";

const mongod = MongoRunner.runMongod();
const db = mongod.getDB("test");
const coll = db.validate_js_bson;
coll.drop();

assert.commandWorked(coll.insert({_id: 1, x: 1}));

function runFunction(returnExpr) {
    return db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{
            $project: {
                result: {
                    $function: {
                        body: "function() { return " + returnExpr + "; }",
                        args: [],
                        lang: "js",
                    }
                }
            }
        }],
        cursor: {}
    });
}

// --------------------------------------------------------------------------
// Cases that must be rejected.
// --------------------------------------------------------------------------

// Encodes a JS number as a little-endian int32 hex string for splicing into a HexData() literal.
// `n >>> 0` normalizes negative/out-of-int32-range inputs to their unsigned 32-bit bit pattern
// before writing, so callers can pass values like -1 or -(2 ** 31) directly.
function int32ToLEHexStr(n) {
    const buf = new ArrayBuffer(4);
    new DataView(buf).setUint32(0, n >>> 0, /* littleEndian */ true);
    return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// Mirrors BSONObjMaxUserSize (src/mongo/bson/util/builder.h)
const kBSONObjMaxUserSize = 16 * 1024 * 1024;

const rejectCases = [
    {
        expr: "new BinData(7, 'Ag==')",
        code: ErrorCodes.InvalidBSONFromJavaScript,
        desc: "malformed BSONColumn (subtype 7) with single 0x02 byte",
    },
];

// A BSONColumn (BinData subtype 7) can hold literal BSONElements. Several element types carry an
// embedded int32 length prefix that the column decompressor trusts. We synthesize such columns via
// $function and require the server to reject the malformed BSON.
//
// oobLength values, and why each is here:
//  - kBSONObjMaxUserSize: the largest size BSON allowed for non-system collections.
//  - a value well inside that cap (1 MiB): proves the size cap doesn't bound the read against the
//    column's actual remaining bytes anywhere in the gap, not just at the edge.
//  - INT32_MIN: a negative length that casts to a huge size_t and should already be handled.
//  - -10..0: a dense sweep of non-positive lengths.
//  - INT32_MAX-10..INT32_MAX+10: a dense sweep around maximum values, including overflow.
for (const oobLength of [
    kBSONObjMaxUserSize,
    1024 * 1024,
    -(2 ** 31),
    ...Array.from({length: 11}, (_, i) => i - 10),
    ...Array.from({length: 21}, (_, i) => 2 ** 31 - 11 + i),
]) {
    const lenHex = int32ToLEHexStr(oobLength);

    // String (0x02), Code (0x0D) and Symbol (0x0E) share the same wire format: an int32 length
    // prefix followed by the NUL-terminated string content. DBPointer (0x0C) prepends the same
    // string descriptor and appends a 12-byte OID pointer. CodeWScope (0x0F) wraps an int32
    // total-size prefix around the string descriptor and a trailing scope document. The inner
    // string length in invalid while keeping the scope object otherwise well-formed (empty).
    //
    // Layout per literal (fieldname is a single empty NUL):
    //   <type> | 0x00 fieldname | <type-specific body> | 0x00 EOO
    const literalCases = [
        {typeHexStr: "02", desc: "String", bodyHex: `${lenHex}7800`},
        {typeHexStr: "0D", desc: "Code", bodyHex: `${lenHex}7800`},
        {typeHexStr: "0E", desc: "Symbol", bodyHex: `${lenHex}7800`},
        {typeHexStr: "0C", desc: "DBPointer", bodyHex: `${lenHex}7800${"12".repeat(12)}`},
        {
            typeHexStr: "0F",
            desc: "CodeWScope",
            // <int32 total size> <int32 string len=oob> "x\0" <empty scope obj: 05 00 00 00 00>
            bodyHex: `${int32ToLEHexStr(4 + 4 + 2 + 5)}${lenHex}78000500000000`,
        },
    ];

    for (const {typeHexStr, desc, bodyHex} of literalCases) {
        const b64 = HexData(7, `${typeHexStr}00${bodyHex}00`).base64();
        rejectCases.push({
            expr: `new BinData(7, '${b64}')`,
            code: ErrorCodes.InvalidBSONFromJavaScript,
            desc: `malformed BSONColumn (subtype 7) with ${desc} literal (type 0x${typeHexStr}) length ${oobLength}`,
        });
    }
}

rejectCases.forEach(function(tc) {
    assert.commandFailedWithCode(runFunction(tc.expr), tc.code, tc.desc);
});

// --------------------------------------------------------------------------
// Cases that must succeed.
// --------------------------------------------------------------------------
const acceptCases = [
    {expr: "42", desc: "number"},
    {expr: "'hello'", desc: "string"},
    {expr: "true", desc: "boolean"},
    {expr: "null", desc: "null"},
    {expr: "new BinData(0, 'AAAA')", desc: "BinData general (subtype 0)"},
    {expr: "new BinData(4, 'AAAAAAAAAAAAAAAAAAAAAA==')", desc: "valid UUID (16 bytes)"},
    {expr: "new BinData(5, 'AAAAAAAAAAAAAAAAAAAAAA==')", desc: "valid MD5 (16 bytes)"},
    {expr: "{a: 1, b: 'two'}", desc: "plain object"},
    {expr: "[1, 2, 3]", desc: "array"},
];

acceptCases.forEach(function(tc) {
    assert.commandWorked(runFunction(tc.expr), tc.desc);
});

// --------------------------------------------------------------------------
// Verify rejection across different pipeline contexts.
// --------------------------------------------------------------------------
const contextCases = [
    {
        desc: "$addFields with $function returning malformed BSONColumn",
        cmd: {
            aggregate: coll.getName(),
            pipeline: [{
                $addFields: {
                    result: {
                        $function: {
                            body: "function() { return new BinData(7, 'Ag=='); }",
                            args: [],
                            lang: "js",
                        }
                    }
                }
            }],
            cursor: {}
        },
    },
    {
        desc: "update pipeline with $set + $function returning malformed BSONColumn",
        cmd: {
            update: coll.getName(),
            updates: [{
                q: {_id: 1},
                u: [{
                    $set: {
                        result: {
                            $function: {
                                body: "function() { return new BinData(7, 'Ag=='); }",
                                args: [],
                                lang: "js",
                            }
                        }
                    }
                }],
                multi: false,
            }]
        },
    },
];

contextCases.forEach(function(tc) {
    assert.commandFailedWithCode(db.runCommand(tc.cmd), ErrorCodes.InvalidBSONFromJavaScript, tc.desc);
});

// After all failed updates, the document must be untouched.
const doc = coll.findOne({_id: 1});
assert(!doc.hasOwnProperty("result"), "document should not have been modified by failed updates");

// --------------------------------------------------------------------------
// Timeseries: malformed BSONColumn cannot be written into bucket data fields
// via $function on system.buckets.
// --------------------------------------------------------------------------
{
    const tsColl = db.validate_js_bson_ts;
    tsColl.drop();

    assert.commandWorked(db.createCollection(
        tsColl.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
    assert.commandWorked(tsColl.insert({t: new Date(), m: 1, v: 42}));

    const bucketsColl = db.getCollection("system.buckets." + tsColl.getName());
    const bucket = bucketsColl.findOne();
    assert.neq(bucket, null, "expected at least one bucket");

    // Attempt to overwrite the time column with malformed BSONColumn via $function.
    const updateRes = db.runCommand({
        update: bucketsColl.getName(),
        updates: [{
            q: {_id: bucket._id},
            u: [{
                $set: {
                    "data.t": {
                        $function: {
                            body: "function() { return new BinData(7, 'Ag=='); }",
                            args: [],
                            lang: "js",
                        }
                    }
                }
            }],
            multi: false,
        }]
    });
    assert.commandFailedWithCode(
        updateRes,
        ErrorCodes.InvalidBSONFromJavaScript,
        "bucket update with $function returning malformed BSONColumn should be rejected");

    // Verify the bucket was not corrupted.
    const bucketAfter = bucketsColl.findOne({_id: bucket._id});
    assert.eq(
        bsonWoCompare(bucket.data.t, bucketAfter.data.t),
        0,
        "bucket data.t should not have been modified");

    // Verify the timeseries collection is still queryable.
    const queryRes = tsColl.find({t: {$gt: new Date(0)}}).toArray();
    assert.eq(queryRes.length, 1, "timeseries query should still return the original document");
}

// --------------------------------------------------------------------------
// $function returning malformed BSON in a simple aggregation is rejected.
// --------------------------------------------------------------------------
{
    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: coll.getName(),
            pipeline: [{
                $project: {
                    result: {
                        $function: {
                            body: "function() { return new BinData(7, 'Ag=='); }",
                            args: [],
                            lang: "js",
                        }
                    }
                }
            }],
            cursor: {}
        }),
        ErrorCodes.InvalidBSONFromJavaScript,
        "$function returning malformed BSONColumn should be rejected");
}

// --------------------------------------------------------------------------
// mapReduce: malformed BSON emitted via objectwrapper::toBSON is rejected.
// --------------------------------------------------------------------------
{
    const mrColl = db.validate_js_bson_mr;
    mrColl.drop();
    assert.commandWorked(mrColl.insert({_id: 1, x: 1}));

    assert.commandFailedWithCode(
        db.runCommand({
            mapReduce: mrColl.getName(),
            map: function() {
                emit(this._id, new BinData(7, 'Ag=='));
            },
            reduce: function(key, values) {
                return values[0];
            },
            out: {inline: 1},
        }),
        ErrorCodes.InvalidBSONFromJavaScript,
        "mapReduce emitting malformed BSONColumn via toBSON should be rejected");
}

MongoRunner.stopMongod(mongod);
})();
