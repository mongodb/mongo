// Tests rewrites for $match expression fields in change streams in combination with special
// characters inside the field path.
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

// Collection name with special characters inside it.
const kCollName = "booooo '\"bar baz]]]}}}}{{{{{{{{{{{{{{{{{{{{{{{{{";

function runTest(inserts, pipelineStages, expectedChanges) {
    assertDropAndRecreateCollection(db, kCollName);

    const cst = new ChangeStreamTest(db);
    const pipeline = [{$changeStream: {}}].concat(pipelineStages);

    jsTestLog("Running test with pipeline: " + tojson(pipeline));
    let cursor = cst.startWatchingChanges({pipeline, collection: kCollName});
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    assert.commandWorked(db[kCollName].insert(inserts));
    cst.assertNextChangesEqual({cursor, expectedChanges});
    cst.cleanUp();
}

// Pipeline without any match expressions.
runTest(
    [
        {_id: "one", value: 1},
        {_id: {sub: "two"}, value: 2},
    ],
    [],
    [
        {
            operationType: "insert",
            fullDocument: {_id: "one", value: 1},
            ns: {db: "test", coll: kCollName},
            documentKey: {_id: "one"},
        },
        {
            operationType: "insert",
            fullDocument: {_id: {sub: "two"}, value: 2},
            ns: {db: "test", coll: kCollName},
            documentKey: {_id: {sub: "two"}},
        },
    ]);

// Value with characters in it that can a have special meaning inside strings.
const fieldValue = "fx {{{\"'x\\";

// Pipeline with literal match expression on _id with special value.
runTest(
    [
        {_id: fieldValue},
    ],
    [
        {$match: {"fullDocument._id": fieldValue}},
    ],
    [
        {
            operationType: "insert",
            fullDocument: {_id: fieldValue},
            ns: {db: "test", coll: kCollName},
            documentKey: {_id: fieldValue},
        },
    ]);

// Field name with characters in it that can a have special meaning inside strings.
const subField = "fx {{{\"'x";

// Pipeline with $expr match expression on $fullDocument _id with subfield that has a special name.
runTest(
    [
        {_id: {[subField]: "x"}},
    ],
    [
        {$match: {["fullDocument._id." + subField]: "x"}},
    ],
    [
        {
            operationType: "insert",
            fullDocument: {_id: {[subField]: "x"}},
            ns: {db: "test", coll: kCollName},
            documentKey: {_id: {[subField]: "x"}},
        },
    ]);

// Pipeline with $expr match expression on $fullDocument _id with subfield that has a special name.
runTest(
    [
        {_id: {[subField]: "x"}},
    ],
    [
        {$match: {$expr: {$eq: ["$fullDocument._id", {[subField]: "x"}]}}},
    ],
    [
        {
            operationType: "insert",
            fullDocument: {_id: {[subField]: "x"}},
            ns: {db: "test", coll: kCollName},
            documentKey: {_id: {[subField]: "x"}},
        },
    ]);

// Pipeline with $expr match expression on $fullDocument _id with subfield that has a special name.
runTest(
    [
        {_id: {[subField]: "x"}},
    ],
    [
        {$match: {$expr: {$eq: ["$fullDocument._id." + subField, "x"]}}},
    ],
    [
        {
            operationType: "insert",
            fullDocument: {_id: {[subField]: "x"}},
            ns: {db: "test", coll: kCollName},
            documentKey: {_id: {[subField]: "x"}},
        },
    ]);

// Pipeline with $expr match expression on subfield that has a special name.
runTest(
    [
        {_id: "z", [subField]: "y"},
    ],
    [
        {$match: {$expr: {$eq: ["$fullDocument." + subField, "y"]}}},
    ],
    [
        {
            operationType: "insert",
            fullDocument: {_id: "z", [subField]: "y"},
            ns: {db: "test", coll: kCollName},
            documentKey: {_id: "z"},
        },
    ]);

// Pipeline with $expr match expression on $documentKey subfield that has a special name.
runTest(
    [
        {_id: {[subField]: "x"}},
    ],
    [
        {$match: {$expr: {$not: {$eq: ["$documentKey." + subField, "x"]}}}},
    ],
    [
        {
            operationType: "insert",
            fullDocument: {_id: {[subField]: "x"}},
            ns: {db: "test", coll: kCollName},
            documentKey: {_id: {[subField]: "x"}},
        },
    ]);

// Pipeline with $expr match expression on $documentKey subfield that has a special name.
runTest(
    [
        {_id: {[subField]: "x"}},
    ],
    [
        {$match: {$expr: {$lte: ["$documentKey." + subField, "a"]}}},
    ],
    [
        {
            operationType: "insert",
            fullDocument: {_id: {[subField]: "x"}},
            ns: {db: "test", coll: kCollName},
            documentKey: {_id: {[subField]: "x"}},
        },
    ]);

// Pipeline with $expr match expression on $documentKey subfield opening 1K braces.
runTest(
    [
        {_id: {[subField]: "x"}},
    ],
    [
        {
            $match: {
                $expr: {
                    $not: {
                        $eq: [
                            "$documentKey.abc', 'test': " +
                                "{a: ".repeat(2048),
                            "x"
                        ]
                    }
                }
            }
        },
    ],
    []);
