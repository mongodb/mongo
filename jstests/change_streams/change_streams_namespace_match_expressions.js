// Tests regular expressions for namespace matching in change streams.
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

// Database name with special characters inside it.
// According to https://www.mongodb.com/docs/manual/reference/limits/#naming-restrictions, many
// special characters are disallowed here.
const kDBName = jsTestName() + "]]][}}{{{{{{{{";
// Collection name with special characters inside it.
const kCollName = "booooo '\"bar baz]]][}}}}\\{{{{{{{{{{{{{{{{{{{{{{{{{";
const kCollNameOther = "other '\"]]]]";

const testDB = db.getSiblingDB(kDBName);

function runTest(db, collection, runOperations, pipelineStages, expectedChanges) {
    assertDropAndRecreateCollection(db, kCollName);
    assertDropAndRecreateCollection(db, kCollNameOther);

    const cst = new ChangeStreamTest(db);
    const pipeline = [{$changeStream: {}}].concat(pipelineStages);

    jsTestLog("Running test with pipeline: " + tojson(pipeline));
    let cursor = cst.startWatchingChanges({pipeline, collection});
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    runOperations(db);

    cst.assertNextChangesEqual({cursor, expectedChanges});
    cst.cleanUp();
}

// Pipeline using a string literal to filter on the entire namespace.
runTest(testDB,
        kCollName,
        (db) => {
            assert.commandWorked(db[kCollName].insert({_id: "test"}));
            assert.commandWorked(db[kCollName].updateOne({_id: "test"}, {$set: {updated: true}}));
            assert.commandWorked(db[kCollName].deleteOne({_id: "test"}));
        },
        [
            {$match: {"ns": {db: kDBName, coll: kCollName}}},
        ],
        [
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "update",
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
                updateDescription: {
                    updatedFields: {
                        updated: true,
                    },
                    removedFields: [],
                    truncatedArrays: [],
                },
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
        ]);

// Pipeline using a string literal to filter on the database name.
runTest(testDB,
        kCollName,
        (db) => {
            assert.commandWorked(db[kCollName].insert({_id: "test"}));
            assert.commandWorked(db[kCollName].deleteOne({_id: "test"}));
        },
        [
            {$match: {"ns.db": kDBName}},
        ],
        [
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
        ]);

// Pipeline using a regular expression to filter on the database name.
runTest(testDB,
        kCollName,
        (db) => {
            assert.commandWorked(db[kCollName].insert({_id: "test"}));
            assert.commandWorked(db[kCollName].deleteOne({_id: "test"}));
        },
        [
            {$match: {"ns.db": new RegExp(RegExp.escape(kDBName))}},
        ],
        [
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
        ]);

// Pipeline using a regular expression to filter on the database name.
runTest(testDB,
        kCollName,
        (db) => {
            assert.commandWorked(db[kCollName].insert({_id: "test"}));
            assert.commandWorked(db[kCollName].deleteOne({_id: "test"}));
        },
        [
            {$match: {"ns.db": new RegExp(RegExp.escape(kDBName) + "[^'\"\\ {}]*")}},
        ],
        [
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
        ]);

// Pipeline using a literal string to filter on the collection name.
runTest(testDB,
        kCollName,
        (db) => {
            assert.commandWorked(db[kCollName].insert({_id: "test"}));
            assert.commandWorked(db[kCollName].deleteOne({_id: "test"}));
        },
        [
            {$match: {"ns.coll": kCollName}},
        ],
        [
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
        ]);

// Pipeline using a regular expression to filter on the collection name.
runTest(testDB,
        kCollName,
        (db) => {
            assert.commandWorked(db[kCollName].insert({_id: "test"}));
            assert.commandWorked(db[kCollNameOther].insert({_id: "test"}));
            assert.commandWorked(db[kCollName].deleteOne({_id: "test"}));
            assert.commandWorked(db[kCollNameOther].deleteOne({_id: "test"}));
        },
        [
            {$match: {"ns.coll": new RegExp(RegExp.escape(kCollName))}},
        ],
        [
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
        ]);

// Pipeline using a regular expression to filter on the collection name.
runTest(testDB,
        kCollName,
        (db) => {
            assert.commandWorked(db[kCollName].insert({_id: "test"}));
            assert.commandWorked(db[kCollNameOther].insert({_id: "test"}));
            assert.commandWorked(db[kCollName].deleteOne({_id: "test"}));
            assert.commandWorked(db[kCollNameOther].deleteOne({_id: "test"}));
        },
        [
            {$match: {"ns.coll": new RegExp("^" + RegExp.escape(kCollName) + "[a-zA-Z]*")}},
        ],
        [
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
        ]);

runTest(testDB,
        kCollName,
        (db) => {
            assert.commandWorked(db[kCollName].insert({_id: "test"}));
            assert.commandWorked(db[kCollNameOther].insert({_id: "test"}));
            assert.commandWorked(db[kCollName].deleteOne({_id: "test"}));
            assert.commandWorked(db[kCollNameOther].deleteOne({_id: "test"}));
        },
        [
            {$match: {"ns.coll": new RegExp(RegExp.escape(kCollName) + "[^'\"\\ {}]*")}},
        ],
        [
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
        ]);

// Pipeline using a regular expression to filter on multiple collections.
runTest(testDB,
        1 /* Use entire database */,
        (db) => {
            assert.commandWorked(db[kCollName].insert({_id: "test"}));
            assert.commandWorked(db[kCollNameOther].insert({_id: "test"}));
            assert.commandWorked(db[kCollName].deleteOne({_id: "test"}));
            assert.commandWorked(db[kCollNameOther].deleteOne({_id: "test"}));
        },
        [
            {
                $match: {
                    "ns.coll": new RegExp("^(" + RegExp.escape(kCollName) + "|" +
                                          RegExp.escape(kCollNameOther) + ")$")
                }
            },
        ],
        [
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollNameOther},
                documentKey: {_id: "test"},
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollNameOther},
                documentKey: {_id: "test"},
            },
        ]);

// Pipeline using a regular expression to filter on multiple collections.
runTest(testDB,
        1 /* Use entire database */,
        (db) => {
            assert.commandWorked(db[kCollName].insert({_id: "test"}));
            assert.commandWorked(db[kCollNameOther].insert({_id: "test"}));
            assert.commandWorked(db[kCollName].deleteOne({_id: "test"}));
            assert.commandWorked(db[kCollNameOther].deleteOne({_id: "test"}));
        },
        [
            {
                $match: {
                    "ns.coll": {
                        $in: [
                            new RegExp("^" + RegExp.escape(kCollName) + "$"),
                            new RegExp("^" + RegExp.escape(kCollNameOther) + "$"),
                        ],
                    }
                }
            },
        ],
        [
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "insert",
                fullDocument: {_id: "test"},
                ns: {db: kDBName, coll: kCollNameOther},
                documentKey: {_id: "test"},
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollName},
                documentKey: {_id: "test"},
            },
            {
                operationType: "delete",
                ns: {db: kDBName, coll: kCollNameOther},
                documentKey: {_id: "test"},
            },
        ]);

assert.commandWorked(testDB.dropDatabase());
