/**
 * Tests that BSON produced by the JavaScript engine ($function) is validated before being used by
 * the server. This prevents malformed BSON from being written to collections or flowing through the
 * aggregation pipeline.
 *
 * @tags: [requires_scripting, requires_fcv_80]
 */
(function () {
    "use strict";

    const mongod = MongoRunner.runMongod();
    const db = mongod.getDB("test");
    const coll = db.validate_js_bson;
    coll.drop();

    assert.commandWorked(coll.insert({_id: 1, x: 1}));

    function runFunction(returnExpr) {
        return db.runCommand({
            aggregate: coll.getName(),
            pipeline: [
                {
                    $project: {
                        result: {
                            $function: {
                                body: "function() { return " + returnExpr + "; }",
                                args: [],
                                lang: "js",
                            },
                        },
                    },
                },
            ],
            cursor: {},
        });
    }

    // --------------------------------------------------------------------------
    // Cases that must be rejected.
    // --------------------------------------------------------------------------
    const rejectCases = [
        {
            expr: "new BinData(7, 'Ag==')",
            code: ErrorCodes.InvalidBSONFromJavaScript,
            desc: "malformed BSONColumn (subtype 7) with single 0x02 byte",
        },
    ];

    rejectCases.forEach(function (tc) {
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

    acceptCases.forEach(function (tc) {
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
                pipeline: [
                    {
                        $addFields: {
                            result: {
                                $function: {
                                    body: "function() { return new BinData(7, 'Ag=='); }",
                                    args: [],
                                    lang: "js",
                                },
                            },
                        },
                    },
                ],
                cursor: {},
            },
        },
        {
            desc: "update pipeline with $set + $function returning malformed BSONColumn",
            cmd: {
                update: coll.getName(),
                updates: [
                    {
                        q: {_id: 1},
                        u: [
                            {
                                $set: {
                                    result: {
                                        $function: {
                                            body: "function() { return new BinData(7, 'Ag=='); }",
                                            args: [],
                                            lang: "js",
                                        },
                                    },
                                },
                            },
                        ],
                        multi: false,
                    },
                ],
            },
        },
    ];

    contextCases.forEach(function (tc) {
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

        assert.commandWorked(db.createCollection(tsColl.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
        assert.commandWorked(tsColl.insert({t: new Date(), m: 1, v: 42}));

        const bucketsColl = db.getCollection("system.buckets." + tsColl.getName());
        const bucket = bucketsColl.findOne();
        assert.neq(bucket, null, "expected at least one bucket");

        // Attempt to overwrite the time column with malformed BSONColumn via $function.
        const updateRes = db.runCommand({
            update: bucketsColl.getName(),
            updates: [
                {
                    q: {_id: bucket._id},
                    u: [
                        {
                            $set: {
                                "data.t": {
                                    $function: {
                                        body: "function() { return new BinData(7, 'Ag=='); }",
                                        args: [],
                                        lang: "js",
                                    },
                                },
                            },
                        },
                    ],
                    multi: false,
                },
            ],
        });
        assert.commandFailedWithCode(
            updateRes,
            ErrorCodes.InvalidBSONFromJavaScript,
            "bucket update with $function returning malformed BSONColumn should be rejected",
        );

        // Verify the bucket was not corrupted.
        const bucketAfter = bucketsColl.findOne({_id: bucket._id});
        assert.eq(bsonWoCompare(bucket.data.t, bucketAfter.data.t), 0, "bucket data.t should not have been modified");

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
                pipeline: [
                    {
                        $project: {
                            result: {
                                $function: {
                                    body: "function() { return new BinData(7, 'Ag=='); }",
                                    args: [],
                                    lang: "js",
                                },
                            },
                        },
                    },
                ],
                cursor: {},
            }),
            ErrorCodes.InvalidBSONFromJavaScript,
            "$function returning malformed BSONColumn should be rejected",
        );
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
                map: function () {
                    emit(this._id, new BinData(7, "Ag=="));
                },
                reduce: function (key, values) {
                    return values[0];
                },
                out: {inline: 1},
            }),
            ErrorCodes.InvalidBSONFromJavaScript,
            "mapReduce emitting malformed BSONColumn via toBSON should be rejected",
        );
    }

    MongoRunner.stopMongod(mongod);
})();
