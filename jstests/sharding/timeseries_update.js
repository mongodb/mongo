/**
 * Test updates into sharded timeseries collection.
 *
 * @tags: [
 *   requires_fcv_51,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest' helpers.

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s0;

//
// Constants used throughout all tests.
//

const dbName = 'testDB';
const collName = 'coll';
const timeField = "time";
const metaField = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");

//
// Checks for feature flags.
//

if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the sharded time-series collection feature flag is disabled");
    st.stop();
    return;
}

const testDB = mongos.getDB(dbName);
testDB.dropDatabase();
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));

if (!TimeseriesTest.shardedTimeseriesUpdatesAndDeletesEnabled(st.shard0)) {
    // Ensure that the feature flag correctly prevents us from running an update on a sharded
    // timeseries collection.
    assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField, metaField}}));
    const coll = testDB.getCollection(collName);
    assert.commandWorked(coll.createIndex({[timeField]: 1}));
    assert.commandWorked(mongos.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: {[timeField]: 1},
    }));
    assert.commandFailedWithCode(
        testDB.runCommand(
            {update: coll.getName(), updates: [{q: {}, u: {[metaField]: 1}, multi: true}]}),
        [ErrorCodes.NotImplemented, ErrorCodes.InvalidOptions]);
    st.stop();
    return;
}

const arbitraryUpdatesEnabled = TimeseriesTest.arbitraryUpdatesEnabled(st.shard0);

const doc1 = {
    _id: 1,
    [timeField]: dateTime,
    [metaField]: {a: "A", b: "B"}
};
const doc2 = {
    _id: 2,
    [timeField]: dateTime,
    [metaField]: {c: "C", d: 2},
    f: [{"k": "K", "v": "V"}]
};
const doc3 = {
    _id: 3,
    [timeField]: dateTime,
    f: "F"
};
const doc4 = {
    _id: 4,
    [timeField]: dateTime,
    [metaField]: {a: "A", b: "B"},
    f: "F"
};
const doc5 = {
    _id: 5,
    [timeField]: dateTime,
    [metaField]: {a: "A", b: "B", c: "C"}
};

//
// Helper functions.
//

/**
 * Confirms that a set of updates returns the expected set of documents.
 */
function testShardedUpdate({
    insert,
    shardKey,
    updatesMetaFieldInShardKey,
    timeseries,
    initialDocList,
    updates,
    resultDocList,
    n,
    nModified = n,
    letDoc = {},
    failCode,
    ordered = true,
}) {
    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
    assert.commandWorked(testDB.createCollection(collName, {timeseries}));

    const coll = testDB.getCollection(collName);
    assert.commandWorked(coll.createIndex(shardKey));
    assert.commandWorked(insert(coll, initialDocList));

    assert.commandWorked(mongos.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: shardKey,
    }));

    // Updates on timeseries collections are not allowed if no metaField is defined.
    if (!timeseries["metaField"]) {
        failCode = [ErrorCodes.InvalidOptions];
        n = 0;
        nModified = 0;
        resultDocList = initialDocList;
    }

    // Updates on sharded timeseries meta fields are only allowed as long as the field updated is
    // not in the shard key.
    if (updatesMetaFieldInShardKey) {
        failCode = [ErrorCodes.InvalidOptions, 31025];
        n = 0;
        nModified = 0;
        resultDocList = initialDocList;
    }

    const updateCommand = {update: coll.getName(), updates, ordered, let : letDoc};
    const res = failCode ? assert.commandFailedWithCode(testDB.runCommand(updateCommand), failCode)
                         : assert.commandWorked(testDB.runCommand(updateCommand));

    assert.eq(n, res.n);
    assert.eq(nModified, res.nModified);
    assert.eq(initialDocList.length, resultDocList.length);

    resultDocList.forEach(resultDoc => {
        assert.docEq(
            resultDoc,
            coll.findOne({_id: resultDoc._id}),
            "Expected document not found in result collection:" + tojson(coll.find().toArray()));
    });

    assert(coll.drop());
}

/**
 * Wrapper on testShardedUpdate to compose tests with a given shardKey/timeseries combo.
 */
function testUpdates({shardKeyTimeField, shardKeyMetaFieldPath, timeseriesOptions, tests}) {
    // Set up the shard key for the tests.
    let shardKey = {};
    if (shardKeyMetaFieldPath) {
        shardKey[shardKeyMetaFieldPath] = 1;
    }
    if (shardKeyTimeField) {
        shardKey[shardKeyTimeField] = 1;
    }

    // Run a series of updates.
    TimeseriesTest.run((insert) => {
        // Helper lambda which has the extensions field set to:
        // - undefined if we are updating the meta field.
        // - the name of a subfield of meta we are updating.
        // - an array of names of subfields of meta we are updating.
        const checkUpdatesMetaFieldInShardKey = (pathToMetaFieldBeingUpdated) => {
            if ((pathToMetaFieldBeingUpdated === undefined) || !shardKeyMetaFieldPath) {
                // If we do not have the meta field in the shard key, we are able to update it.
                return false;
            } else if ((shardKeyMetaFieldPath === metaField) ||
                       (pathToMetaFieldBeingUpdated === "")) {
                // If the top-level meta field is in the shard key, we cannot update it.
                return true;
            } else if (!Array.isArray(pathToMetaFieldBeingUpdated)) {
                pathToMetaFieldBeingUpdated = [pathToMetaFieldBeingUpdated];
            }
            for (const e of pathToMetaFieldBeingUpdated) {
                if (metaField + "." + e === shardKeyMetaFieldPath) {
                    return true;
                }
            }
            return false;
        };

        const testUpdate = (fields, additionalFields = {}) => {
            let inputs = Object.assign({}, fields, additionalFields);
            testShardedUpdate(Object.assign({}, inputs, {
                shardKey,
                insert,
                timeseries: timeseriesOptions,
                updatesMetaFieldInShardKey:
                    checkUpdatesMetaFieldInShardKey(inputs.pathToMetaFieldBeingUpdated)
            }));
        };

        tests.forEach(test => test({testUpdate}));
    }, testDB);
}

/**
 * Helper function to generate the parameters to pass to 'testUpdates' when an update is expected to
 * fail.
 */
function expectFailedUpdate(initialDocList) {
    return {
        initialDocList,
        resultDocList: initialDocList,
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    };
}

//
// Test cases when the update command fails.
//

function testCaseMultiFalseUpdateFails({testUpdate}) {
    testUpdate({updates: [{q: {[metaField]: {b: "B"}}, u: {$set: {[metaField]: {b: "C"}}}}]},
               expectFailedUpdate([doc1]));
}

function testCaseReplacementAndPipelineUpdateFails({testUpdate}) {
    const expectFailedUpdateDoc = expectFailedUpdate([doc2]);

    // Replace a document to have no metaField, which should fail since updates with replacement
    // documents are not supported.
    testUpdate({
        updates: [{
            q: {[metaField]: {c: "C", d: 2}},
            u: {f2: {e: "E", f: "F"}, f3: 7},
            multi: true,
        }]
    },
               expectFailedUpdateDoc);

    // Replace a document with an empty document, which should fail since updates with replacement
    // documents are not supported.
    testUpdate({
        updates: [{
            q: {[metaField]: {c: "C", d: 2}},
            u: {},
            multi: true,
        }]
    },
               expectFailedUpdateDoc);

    // Modify the metaField, which should fail since pipeline-style updates are not supported.
    testUpdate({
        updates: [{
            q: {},
            u: [
                {$addFields: {[metaField + ".c"]: "C", [metaField + ".e"]: "E"}},
                {$unset: metaField + ".e"}
            ],
            multi: true,
        }]
    },
               expectFailedUpdateDoc);
}

function testCaseNoMetaFieldQueryUpdateFails({testUpdate}) {
    if (arbitraryUpdatesEnabled) {
        return;
    }

    // Query on a field which is not the (nonexistent) metaField.
    testUpdate({
        updates: [{
            q: {f: "F"},
            u: {},
            multi: true,
        }]
    },
               expectFailedUpdate([doc3]));

    // Query on all documents and update them to be empty documents.
    testUpdate({
        updates: [{
            q: {},
            u: {},
            multi: true,
        }]
    },
               expectFailedUpdate([doc3]));

    // Query on all documents and update them to be non-empty documents.
    testUpdate({
        updates: [{
            q: {},
            u: {f: "FF"},
            multi: true,
        }]
    },
               expectFailedUpdate([doc3]));

    // Query on a field that is not the metaField.
    testUpdate({
        updates: [{
            q: {measurement: "cpu"},
            u: {$set: {[metaField]: {c: "C"}}},
            multi: true,
        }]
    },
               expectFailedUpdate([doc1]));

    // Query on the metaField and a field that is not the metaField.
    testUpdate({
        updates: [
            {
                q: {[metaField]: {a: "A", b: "B"}, measurement: "cpu"},
                u: {$set: {[metaField]: {c: "C"}}},
                multi: true,
            },
        ]
    },
               expectFailedUpdate([doc1]));

    // Query on a field that is not the metaField using dot notation and modify the metaField.
    testUpdate({
        updates: [{
            q: {"measurement.A": "cpu"},
            u: {$set: {[metaField]: {c: "C"}}},
            multi: true,
        }]
    },
               expectFailedUpdate([doc1]));
}

function testCaseIllegalMetaFieldUpdateFails({testUpdate}) {
    if (!arbitraryUpdatesEnabled) {
        // Query on the metaField and modify a field that is not the metaField.
        testUpdate({
            updates: [{
                q: {[metaField]: {c: "C", d: 2}},
                u: {$set: {f2: "f2"}},
                multi: true,
            }]
        },
                   expectFailedUpdate([doc2]));

        // Query on the metaField and modify the metaField and fields that are not the metaField.
        testUpdate({
            updates: [{
                q: {[metaField]: {c: "C", d: 2}},
                u: {$set: {[metaField]: {e: "E"}, f3: "f3"}, $inc: {f2: 3}, $unset: {f1: ""}},
                multi: true,
            }]
        },
                   expectFailedUpdate([doc2]));
    }

    // Rename the metaField.
    testUpdate({
        updates: [{
            q: {[metaField + ".a"]: "A"},
            u: {$rename: {[metaField]: "Z"}},
            multi: true,
        }]
    },
               expectFailedUpdate([doc1, doc2, doc4]));

    // Rename a subfield of the metaField to something not in the metaField.
    testUpdate({
        updates: [{
            q: {[metaField + ".a"]: "A"},
            u: {$rename: {[metaField + ".a"]: "notMetaField.a"}},
            multi: true,
        }]
    },
               expectFailedUpdate([doc1, doc2, doc4]));
}

//
// Test cases when the update command succeeds.
//

function testCaseBatchUpdates({testUpdate}) {
    // Multiple updates, ordered: query on the metaField and modify the metaField multiple times.
    testUpdate({
        initialDocList: [doc2],
        updates: [
            {
                q: {[metaField]: {c: "C", d: 2}},
                u: {$set: {[metaField]: 1}},
                multi: true,
            },
            {
                q: {[metaField]: 1},
                u: {$set: {[metaField]: 2}},
                multi: true,
            },
            {
                q: {[metaField]: 2},
                u: {$set: {[metaField]: 3}},
                multi: true,
            }
        ],
        resultDocList: [{
            _id: 2,
            [timeField]: dateTime,
            [metaField]: 3,
            f: [{"k": "K", "v": "V"}],
        }],
        n: 3,
        pathToMetaFieldBeingUpdated: "",
    });

    // Multiple updates, ordered: query on the metaField and on a field that is not the metaField.
    testUpdate({
        initialDocList: [doc1],
        updates: [
            {
                q: {[metaField]: {a: "A", b: "B"}},
                u: {$set: {[metaField]: {c: "C", d: 1}}},
                multi: true,
            },
            {
                q: {measurement: "cpu", [metaField + ".d"]: 1},
                u: {$set: {[metaField + ".c"]: "CC"}},
                multi: true,
            }
        ],
        resultDocList: [doc1],
        // If the shardKey is on one of the fields being updated, this must fail.
        n: 1,
        pathToMetaFieldBeingUpdated: "",
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple updates, ordered: query on the metaField and modify the metaField and a field that
    // is not the metaField using dot notation.
    testUpdate({
        initialDocList: [doc2],
        updates: [
            {
                q: {[metaField]: {c: "C", d: 2}},
                u: {$inc: {[metaField + ".d"]: 6}},
                multi: true,
            },
            {
                q: {[metaField]: {c: "C", d: 8}},
                u: {$set: {"f1.0": "f2"}},
                multi: true,
            }
        ],
        resultDocList: [{
            _id: 2,
            [timeField]: dateTime,
            [metaField]: {c: "C", d: 8},
            f: [{"k": "K", "v": "V"}],
        }],
        // If the shardKey is on the field being updated, this must fail to update any docs.
        n: 1,
        pathToMetaFieldBeingUpdated: "d",
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple updates, ordered: query on the metaField and modify a field that is not the
    // metaField using dot notation.
    if (!arbitraryUpdatesEnabled) {
        testUpdate({
            updates: [
                {
                    q: {[metaField]: {c: "C", d: 2}},
                    u: {$set: {"f1.0": "f2"}},
                    multi: true,
                },
                {
                    q: {[metaField]: {c: "C", d: 2}},
                    u: {$inc: {[metaField + ".d"]: 6}},
                    multi: true,
                }
            ]
        },
                   expectFailedUpdate([doc2]));

        // Multiple updates, unordered: Modify the metaField, a field that is not the metaField, and
        // the metaField. The first and last updates should succeed.
        testUpdate({
            initialDocList: [doc2],
            updates: [
                {
                    q: {[metaField]: {c: "C", d: 2}},
                    u: {$inc: {[metaField + ".d"]: 6}},
                    multi: true,
                },
                {
                    q: {[metaField]: {c: "C", d: 8}},
                    u: {$set: {"f1.0": "f2"}},
                    multi: true,
                },
                {
                    q: {[metaField]: {c: "C", d: 8}},
                    u: {$inc: {[metaField + ".d"]: 7}},
                    multi: true,
                }
            ],
            resultDocList: [{
                _id: 2,
                [timeField]: dateTime,
                [metaField]: {c: "C", d: 15},
                f: [{"k": "K", "v": "V"}],
            }],
            ordered: false,
            n: 2,
            pathToMetaFieldBeingUpdated: "d",
            failCode: ErrorCodes.InvalidOptions,
        });
    }
}

function testCaseValidMetaFieldUpdates({testUpdate}) {
    // Rename a subfield to the metaField.
    testUpdate({
        initialDocList: [doc1, doc2],
        updates: [{
            q: {[metaField + ".a"]: "A"},
            u: {$rename: {[metaField + ".a"]: metaField + ".z"}},
            multi: true,
        }],
        resultDocList: [{_id: 1, [timeField]: dateTime, [metaField]: {z: "A", b: "B"}}, doc2],
        n: 1,
        pathToMetaFieldBeingUpdated: "a",
    });

    // Query on the metaField and modify the metaField.
    testUpdate({
        initialDocList: [doc1],
        updates: [{
            q: {[metaField]: {a: "A", b: "B"}},
            u: {$set: {[metaField]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [{_id: 1, [timeField]: dateTime, [metaField]: {c: "C"}}],
        n: 1,
        pathToMetaFieldBeingUpdated: "",
    });

    // Query on the metaField and modify the metaField of 1 matching document.
    testUpdate({
        initialDocList: [doc1, doc2, doc4, doc5],
        updates: [{
            q: {"$and": [{[metaField + ".c"]: "C"}, {[metaField + ".d"]: 2}]},
            u: {$set: {[metaField + ".c"]: 1}},
            multi: true,
        }],
        resultDocList: [
            doc1,
            {_id: 2, [timeField]: dateTime, [metaField]: {c: 1, d: 2}, f: [{"k": "K", "v": "V"}]},
            doc4,
            doc5
        ],
        ordered: false,
        n: 1,
        pathToMetaFieldBeingUpdated: "c",
    });

    // Query on the metaField and update the metaField of multiple matching documents.
    testUpdate({
        initialDocList: [doc1, doc2, doc4, doc5],
        updates: [{
            q: {[metaField + ".a"]: "A"},
            u: {$unset: {[metaField + ".a"]: ""}},
            multi: true,
        }],
        resultDocList: [
            {_id: 1, [timeField]: dateTime, [metaField]: {b: "B"}},
            {_id: 2, [timeField]: dateTime, [metaField]: {c: "C", d: 2}, f: [{"k": "K", "v": "V"}]},
            {_id: 4, [timeField]: dateTime, [metaField]: {b: "B"}, f: "F"},
            {_id: 5, [timeField]: dateTime, [metaField]: {b: "B", c: "C"}}
        ],
        ordered: false,
        n: 3,
        pathToMetaFieldBeingUpdated: "a",
    });

    // Compound query on the metaField using dot notation and modify the metaField.
    testUpdate({
        initialDocList: [doc1],
        updates: [{
            q: {"$and": [{[metaField + ".a"]: "A"}, {[metaField + ".b"]: "B"}]},
            u: {$set: {[metaField]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [{_id: 1, [timeField]: dateTime, [metaField]: {c: "C"}}],
        n: 1,
        pathToMetaFieldBeingUpdated: "",
    });

    // Query on the metaField using dot notation and modify the metaField.
    testUpdate({
        initialDocList: [doc1],
        updates: [{
            q: {[metaField + ".a"]: "A"},
            u: {$set: {[metaField]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [{_id: 1, [timeField]: dateTime, [metaField]: {c: "C"}}],
        n: 1,
        pathToMetaFieldBeingUpdated: "",
    });

    // Query on the metaField using dot notation and modify the metaField.
    testUpdate({
        initialDocList: [doc2],
        updates: [{
            q: {[metaField + ".c"]: "C"},
            u: {$inc: {[metaField + ".d"]: 10}},
            multi: true,
        }],
        resultDocList: [
            {_id: 2, [timeField]: dateTime, [metaField]: {c: "C", d: 12}, f: [{"k": "K", "v": "V"}]}
        ],
        n: 1,
        pathToMetaFieldBeingUpdated: "d",
    });

    // Query with an empty document (i.e update all documents in the collection).
    testUpdate({
        initialDocList: [doc1, doc2],
        updates: [{
            q: {},
            u: {$set: {[metaField]: {z: "Z"}}},
            multi: true,
        }],
        resultDocList: [
            {_id: 1, [timeField]: dateTime, [metaField]: {z: "Z"}},
            {_id: 2, [timeField]: dateTime, [metaField]: {z: "Z"}, f: [{"k": "K", "v": "V"}]}
        ],
        n: 2,
        pathToMetaFieldBeingUpdated: "",
    });

    // Remove the metaField.
    testUpdate({
        initialDocList: [doc1],
        updates:
            [{q: {[metaField]: {a: "A", b: "B"}}, u: {$unset: {[metaField]: ""}}, multi: true}],
        resultDocList: [{_id: 1, [timeField]: dateTime}],
        n: 1,
        pathToMetaFieldBeingUpdated: "",
    });

    // Update where one of the matching documents is a no-op update.
    testUpdate({
        initialDocList: [doc1, doc4, doc5],
        updates: [
            {
                q: {[metaField + ".a"]: "A"},
                u: {$set: {[metaField + ".c"]: "C"}},
                multi: true,
            },
        ],
        resultDocList: [
            {_id: 1, [timeField]: dateTime, [metaField]: {a: "A", b: "B", c: "C"}},
            {_id: 4, [timeField]: dateTime, [metaField]: {a: "A", b: "B", c: "C"}, f: "F"},
            {_id: 5, [timeField]: dateTime, [metaField]: {a: "A", b: "B", c: "C"}}
        ],
        n: 3,
        nModified: 2,
        pathToMetaFieldBeingUpdated: "c",
    });

    // Query for documents using $jsonSchema with the metaField required.
    testUpdate({
        initialDocList: [doc1, doc2, doc3],
        updates: [{
            q: {"$jsonSchema": {"required": [metaField]}},
            u: {$set: {[metaField]: "a"}},
            multi: true
        }],
        resultDocList: [
            {_id: 1, [timeField]: dateTime, [metaField]: "a"},
            {_id: 2, [timeField]: dateTime, [metaField]: "a", f: [{"k": "K", "v": "V"}]},
            doc3
        ],
        n: 2,
        pathToMetaFieldBeingUpdated: "",
    });

    // Query for documents using $jsonSchema with the metaField in dot notation required.
    testUpdate({
        initialDocList: [doc1, doc2, doc3],
        updates: [{
            q: {"$jsonSchema": {"required": [metaField + ".a"]}},
            u: {$set: {[metaField]: "a"}},
            multi: true
        }],
        resultDocList: [{_id: 1, [timeField]: dateTime, [metaField]: "a"}, doc2, doc3],
        n: 1,
        pathToMetaFieldBeingUpdated: "",
    });

    // Query for documents using $jsonSchema with a field that is not the metaField required.
    testUpdate({
        updates: [{
            q: {"$jsonSchema": {"required": [metaField, timeField]}},
            u: {$set: {[metaField]: "a"}},
            multi: true
        }],
    },
               expectFailedUpdate([doc1, doc2, doc3]));

    const nestedMetaObj = {_id: 6, [timeField]: dateTime, [metaField]: {[metaField]: "A", a: 1}};

    // Query for documents using $jsonSchema with the metaField required and a required subfield of
    // the metaField with the same name as the metaField.
    if (!arbitraryUpdatesEnabled) {
        testUpdate({
            initialDocList: [doc1, nestedMetaObj],
            updates: [{
                q: {
                    "$jsonSchema": {
                        "required": [metaField],
                        "properties": {[metaField]: {"required": [metaField]}}
                    }
                },
                u: {$set: {[metaField]: "a"}},
                multi: true
            }],
            resultDocList: [doc1, {_id: 6, [timeField]: dateTime, [metaField]: "a", a: 1}],
            n: 1,
            pathToMetaFieldBeingUpdated: "",
        });
    }

    // Query for documents using $jsonSchema with the metaField required and an optional field that
    // is not the metaField.
    testUpdate({
        updates: [{
            q: {
                "$jsonSchema": {
                    "required": [metaField],
                    "properties": {"measurement": {description: "can be any value"}}
                }
            },
            u: {$set: {[metaField]: "a"}},
            multi: true
        }]
    },
               expectFailedUpdate([doc1, nestedMetaObj]));

    // Query for documents on the metaField with the metaField nested within nested operators.
    testUpdate({
        initialDocList: [doc1, doc2, doc3],
        updates: [{
            q: {
                "$and": [
                    {"$or": [{[metaField]: {"$ne": "B"}}, {[metaField]: {"a": {"$eq": "B"}}}]},
                    {[metaField]: {a: "A", b: "B"}}
                ]
            },
            u: {$set: {[metaField]: "a"}},
            multi: true
        }],
        resultDocList: [{_id: 1, [timeField]: dateTime, [metaField]: "a"}, doc2, doc3],
        n: 1,
        pathToMetaFieldBeingUpdated: "",
    });

    // Updates where upsert:false should not insert a new document when no match is found. In case
    // the shard key is on the meta field, this should not update any documents but also but not
    // report a failure, since no documents were matched.
    testUpdate({
        initialDocList: [doc1, doc4, doc5],
        updates: [{
            q: {[metaField]: "Z"},
            u: {$set: {[metaField]: 5}},
            multi: true,
        }],
        resultDocList: [doc1, doc4, doc5],
        n: 0,
    });

    // Do the same test case as above but with upsert:true, which should fail.
    testUpdate({
        updates: [{
            q: {[metaField]: "Z"},
            u: {$set: {[metaField]: 5}},
            multi: true,
            upsert: true,
        }]
    },
               expectFailedUpdate([doc1, doc4, doc5]));
}

function testCaseUpdateWithLetDoc({testUpdate}) {
    // Use a variable defined in the let option in the query to modify the metaField.
    testUpdate({
        initialDocList: [doc1, doc4, doc5],
        updates: [{
            q: {$expr: {$eq: ["$" + metaField + ".a", "$$oldVal"]}},
            u: {$set: {[metaField]: "aaa"}},
            multi: true,
        }],
        letDoc: {oldVal: "A"},
        resultDocList: [
            {_id: 1, [timeField]: dateTime, [metaField]: "aaa"},
            {_id: 4, [timeField]: dateTime, [metaField]: "aaa", f: "F"},
            {_id: 5, [timeField]: dateTime, [metaField]: "aaa"}
        ],
        n: 3,
        pathToMetaFieldBeingUpdated: "",
    });

    // Variables defined in the let option can only be used in the update if the update is an
    // pipeline update. Since this update is an update document, the literal name of the variable
    // will be used in the update instead of the variable's value.
    testUpdate({
        initialDocList: [doc1],
        updates: [{
            q: {[metaField + ".a"]: "A"},
            u: {$set: {[metaField]: "$$myVar"}},
            multi: true,
        }],
        letDoc: {myVar: "aaa"},
        resultDocList: [{_id: 1, [timeField]: dateTime, [metaField]: "$$myVar"}],
        n: 1,
        pathToMetaFieldBeingUpdated: "",
    });

    // Use variables defined in the let option in the query to modify the metaField multiple times.
    testUpdate({
        initialDocList: [doc1, doc4, doc5],
        updates: [
            {
                q: {$expr: {$eq: ["$" + metaField + ".a", "$$val1"]}},
                u: {$set: {[metaField]: "aaa"}},
                multi: true,
            },
            {
                q: {$expr: {$eq: ["$" + metaField, "$$val2"]}},
                u: {$set: {[metaField]: "bbb"}},
                multi: true,
            }
        ],
        letDoc: {val1: "A", val2: "aaa"},
        resultDocList: [
            {_id: 1, [timeField]: dateTime, [metaField]: "bbb"},
            {_id: 4, [timeField]: dateTime, [metaField]: "bbb", f: "F"},
            {_id: 5, [timeField]: dateTime, [metaField]: "bbb"}
        ],
        n: 6,
        pathToMetaFieldBeingUpdated: "",
    });
}

function testCaseCollationUpdates({testUpdate}) {
    const collationDoc1 = {_id: 1, [timeField]: dateTime, [metaField]: "café"};
    const collationDoc2 = {_id: 2, [timeField]: dateTime, [metaField]: "cafe"};
    const collationDoc3 = {_id: 3, [timeField]: dateTime, [metaField]: "cafE"};
    const initialDocList = [collationDoc1, collationDoc2, collationDoc3];

    // Query on the metaField and modify the metaField using collation with strength level 1.
    testUpdate({
        initialDocList,
        updates: [{
            q: {[metaField]: "cafe"},
            u: {$set: {[metaField]: "Updated"}},
            multi: true,
            collation: {locale: "fr", strength: 1},
        }],
        resultDocList: [
            {_id: 1, [timeField]: dateTime, [metaField]: "Updated"},
            {_id: 2, [timeField]: dateTime, [metaField]: "Updated"},
            {_id: 3, [timeField]: dateTime, [metaField]: "Updated"}
        ],
        n: 3,
        pathToMetaFieldBeingUpdated: "",
    });

    // Query on the metaField and modify the metaField using collation with the default strength
    // (level 3).
    testUpdate({
        initialDocList,
        updates: [{
            q: {[metaField]: "cafe"},
            u: {$set: {[metaField]: "Updated"}},
            multi: true,
            collation: {locale: "fr"},
        }],
        resultDocList: [
            collationDoc1,
            {_id: 2, [timeField]: dateTime, [metaField]: "Updated"},
            collationDoc3,
        ],
        n: 1,
        pathToMetaFieldBeingUpdated: "",
    });
}

function testCaseNullUpdates({testUpdate}) {
    // Assumes shard key is meta.a.
    const nullDoc = {_id: 1, [timeField]: dateTime, [metaField]: {a: null, b: 1}};
    const missingDoc1 = {_id: 2, [timeField]: dateTime, [metaField]: {b: 1}};
    const missingDoc2 = {_id: 3, [timeField]: dateTime, [metaField]: "foo"};
    const initialDocList = [nullDoc, missingDoc1, missingDoc2];

    // Query on the metaField and modify the metaField using collation with strength level 1.
    testUpdate({
        initialDocList,
        updates: [{
            q: {[metaField]: {$ne: null}},
            u: {$set: {[metaField]: "Updated"}},
            multi: true,
        }],
        resultDocList: [
            {_id: 1, [timeField]: dateTime, [metaField]: "Updated"},
            {_id: 2, [timeField]: dateTime, [metaField]: "Updated"},
            {_id: 3, [timeField]: dateTime, [metaField]: "Updated"},
        ],
        n: 3,
    });
}

// Run tests with a variety of shardKeys and timeseries configurations.
const timeseriesOptions = {
    timeField,
    metaField
};
const tests = [
    testCaseMultiFalseUpdateFails,
    testCaseNoMetaFieldQueryUpdateFails,
    testCaseIllegalMetaFieldUpdateFails,
    testCaseReplacementAndPipelineUpdateFails,
    testCaseCollationUpdates,
    testCaseUpdateWithLetDoc,
    testCaseBatchUpdates,
    testCaseValidMetaFieldUpdates,
];
if (!arbitraryUpdatesEnabled) {
    testUpdates({shardKeyTimeField: timeField, timeseriesOptions: {timeField}, tests});
}
testUpdates({shardKeyMetaFieldPath: metaField, timeseriesOptions, tests});
testUpdates(
    {shardKeyTimeField: timeField, shardKeyMetaFieldPath: metaField, timeseriesOptions, tests});

// Run a relevant subset of tests in the case when meta.a is the shard key.
const testsForMetaSubfieldShardKey = [
    testCaseNullUpdates,
    testCaseMultiFalseUpdateFails,
    testCaseNoMetaFieldQueryUpdateFails,
    testCaseIllegalMetaFieldUpdateFails,
    testCaseReplacementAndPipelineUpdateFails,
    testCaseValidMetaFieldUpdates,
];
testUpdates({
    shardKeyMetaFieldPath: metaField + ".a",
    timeseriesOptions,
    tests: testsForMetaSubfieldShardKey,
});
testUpdates({
    shardKeyMetaFieldPath: metaField + ".a",
    shardKeyTimeField: timeField,
    timeseriesOptions,
    tests: testsForMetaSubfieldShardKey,
});

st.stop();
})();
