/**
 * Tests that BSON produced by the JavaScript engine ($function) is validated before being used by
 * the server. This prevents malformed BSON from being written to collections or flowing through the
 * aggregation pipeline.
 *
 * @tags: [requires_scripting, requires_fcv_90]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const malformedBSONColumn = "new BinData(7, 'Ag==')";
const validBSONColumn =
    "new BinData(7, 'AQAAAAAAAAAAQJN/AAAAAAAAAAIAAAAAAAAABwAAAAAAAAAOAAAAAAAAAAA=')";

const acceptCases = [
    {expr: "42", desc: "number"},
    {expr: "'hello'", desc: "string"},
    {expr: "true", desc: "boolean"},
    {expr: "null", desc: "null"},
    {expr: "new BinData(0, 'AAAA')", desc: "BinData general (subtype 0)"},
    {expr: "new BinData(4, 'AAAAAAAAAAAAAAAAAAAAAA==')", desc: "valid UUID (16 bytes)"},
    {expr: "new BinData(5, 'AAAAAAAAAAAAAAAAAAAAAA==')", desc: "valid MD5 (16 bytes)"},
    {
        expr: "new BinData(7, 'AQAAAAAAAAAAQJN/AAAAAAAAAAIAAAAAAAAABwAAAAAAAAAOAAAAAAAAAAA=')",
        desc: "valid BSONColumn (subtype 7)",
    },
    {expr: "{a: 1, b: 'two'}", desc: "plain object"},
    {expr: "[1, 2, 3]", desc: "array"},
];

function makeFunctionBody(returnExpr) {
    return "function() { return " + returnExpr + "; }";
}

function makeFunctionExpr(returnExpr) {
    return {$function: {body: makeFunctionBody(returnExpr), args: [], lang: "js"}};
}

// Encodes a JS number as a little-endian int32 hex string for splicing into a HexData() literal.
function int32ToLEHexStr(n) {
    const buf = new ArrayBuffer(4);
    new DataView(buf).setUint32(0, n >>> 0, /* littleEndian */ true);
    return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

const kBSONObjMaxUserSize = 16 * 1024 * 1024;

const rejectCases = [
    {
        desc: "$project with malformed BSONColumn",
        cmd: (db, coll) =>
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$project: {result: makeFunctionExpr(malformedBSONColumn)}}],
                cursor: {},
            }),
    },
    {
        desc: "$addFields with malformed BSONColumn",
        cmd: (db, coll) =>
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$addFields: {result: makeFunctionExpr(malformedBSONColumn)}}],
                cursor: {},
            }),
    },
    {
        desc: "update pipeline with $set returning malformed BSONColumn",
        cmd: (db, coll) =>
            db.runCommand({
                update: coll.getName(),
                updates: [
                    {
                        q: {_id: 1},
                        u: [{$set: {result: makeFunctionExpr(malformedBSONColumn)}}],
                        multi: false,
                    },
                ],
            }),
    },
    {
        desc: "mapReduce emitting malformed BSONColumn",
        cmd: (db, _coll) => {
            const mrColl = db.validate_js_bson_mr;
            mrColl.drop();
            assert.commandWorked(mrColl.insert({_id: 1, x: 1}));
            const res = db.runCommand({
                mapReduce: mrColl.getName(),
                map: function () {
                    emit(this._id, new BinData(7, "Ag=="));
                },
                reduce: function (key, values) {
                    return values[0];
                },
                out: {inline: 1},
            });
            mrColl.drop();
            return res;
        },
    },
];

// A BSONColumn (BinData subtype 7) can hold literal BSONElements. Several element types carry an
// embedded int32 length prefix that the column decompressor trusts. We synthesize such columns via
// $function and require the server to reject the malformed BSON.
for (const oobLength of [
    kBSONObjMaxUserSize,
    1024 * 1024,
    -(2 ** 31),
    ...Array.from({length: 11}, (_, i) => i - 10),
    ...Array.from({length: 21}, (_, i) => 2 ** 31 - 11 + i),
]) {
    const lenHex = int32ToLEHexStr(oobLength);

    const literalCases = [
        {typeHexStr: "02", desc: "String", bodyHex: `${lenHex}7800`},
        {typeHexStr: "0D", desc: "Code", bodyHex: `${lenHex}7800`},
        {typeHexStr: "0E", desc: "Symbol", bodyHex: `${lenHex}7800`},
        {typeHexStr: "0C", desc: "DBPointer", bodyHex: `${lenHex}7800${"12".repeat(12)}`},
        {
            typeHexStr: "0F",
            desc: "CodeWScope",
            bodyHex: `${int32ToLEHexStr(4 + 4 + 2 + 5)}${lenHex}78000500000000`,
        },
    ];

    for (const {typeHexStr, desc, bodyHex} of literalCases) {
        const b64 = HexData(7, `${typeHexStr}00${bodyHex}00`).base64();
        const expr = `new BinData(7, '${b64}')`;
        rejectCases.push({
            desc: `malformed BSONColumn with ${desc} literal (0x${typeHexStr}) length ${oobLength}`,
            cmd: (db, coll) =>
                db.runCommand({
                    aggregate: coll.getName(),
                    pipeline: [{$project: {result: makeFunctionExpr(expr)}}],
                    cursor: {},
                }),
        });
    }
}

describe("validate JS-generated BSON", function () {
    before(function () {
        // FTDC samples can exceed the intentionally low BSON memory limit used below.
        this.mongod = MongoRunner.runMongod({
            setParameter: {diagnosticDataCollectionEnabled: false},
        });
        this.db = this.mongod.getDB("test");
        this.coll = this.db.validate_js_bson;
        this.coll.drop();
        assert.commandWorked(this.coll.insert({_id: 1, x: 1}));
    });

    after(function () {
        if (this.mongod) {
            MongoRunner.stopMongod(this.mongod);
        }
    });

    for (const tc of acceptCases) {
        it("accepts " + tc.desc, function () {
            assert.commandWorked(
                this.db.runCommand({
                    aggregate: this.coll.getName(),
                    pipeline: [{$project: {result: makeFunctionExpr(tc.expr)}}],
                    cursor: {},
                }),
                tc.desc,
            );
        });
    }

    for (const tc of rejectCases) {
        it("rejects " + tc.desc, function () {
            assert.commandFailedWithCode(
                tc.cmd(this.db, this.coll),
                ErrorCodes.InvalidBSONFromJavaScript,
                tc.desc,
            );
        });
    }

    it("does not modify document on failed update", function () {
        assert.commandFailedWithCode(
            this.db.runCommand({
                update: this.coll.getName(),
                updates: [
                    {
                        q: {_id: 1},
                        u: [{$set: {result: makeFunctionExpr(malformedBSONColumn)}}],
                        multi: false,
                    },
                ],
            }),
            ErrorCodes.InvalidBSONFromJavaScript,
        );
        const doc = this.coll.findOne({_id: 1});
        assert.docEq(
            {_id: 1, x: 1},
            doc,
            "document should not have been modified by failed updates",
        );
    });

    it("rejects malformed BSONColumn in timeseries bucket update", function () {
        const tsColl = this.db.validate_js_bson_ts;
        tsColl.drop();

        assert.commandWorked(
            this.db.createCollection(tsColl.getName(), {
                timeseries: {timeField: "t", metaField: "m"},
            }),
        );
        assert.commandWorked(tsColl.insert({t: new Date(), m: 1, v: 42}));

        const bucketsColl = getTimeseriesCollForRawOps(this.db, tsColl);
        const rawSpec = getRawOperationSpec(this.db);
        const rawBuckets = bucketsColl.aggregate([{$match: {}}], rawSpec).toArray();
        assert.gt(rawBuckets.length, 0, "expected at least one bucket");
        const bucketBefore = rawBuckets[0];

        const docsBefore = tsColl.find().toArray();

        assert.commandFailedWithCode(
            this.db.runCommand(
                Object.assign(
                    {
                        update: bucketsColl.getName(),
                        updates: [
                            {
                                q: {_id: bucketBefore._id},
                                u: [{$set: {"data.t": makeFunctionExpr(malformedBSONColumn)}}],
                                multi: false,
                            },
                        ],
                    },
                    rawSpec,
                ),
            ),
            ErrorCodes.InvalidBSONFromJavaScript,
            "bucket update with $function returning malformed BSONColumn should be rejected",
        );

        const bucketsAfter = bucketsColl
            .aggregate([{$match: {_id: bucketBefore._id}}], rawSpec)
            .toArray();
        assert.eq(bucketsAfter.length, 1, "expected bucket to still exist");
        assert.docEq(bucketBefore, bucketsAfter[0], "bucket should not have been modified");

        const docsAfter = tsColl.find().toArray();
        assert.eq(
            docsBefore.length,
            docsAfter.length,
            "timeseries document count should be unchanged",
        );
        assert.docEq(docsBefore[0], docsAfter[0], "timeseries document should be unchanged");

        tsColl.drop();
    });
});
