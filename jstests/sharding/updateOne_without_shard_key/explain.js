/**
 * Test explain output for updateOne, deleteOne, and findAndModify without shard key.
 *
 * @tags: [
 *    requires_sharding,
 *    requires_fcv_71,
 *    featureFlagUpdateOneWithoutShardKey,
 * ]
 */

(function() {
"use strict";

load("jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js");

// 2 shards single node, 1 mongos, 1 config server 3-node.
const st = new ShardingTest({});
const dbName = "testDb";
const collName = "testColl";
const nss = dbName + "." + collName;
const splitPoint = 0;
const dbConn = st.s.getDB(dbName);
const docsToInsert = [
    {_id: 0, x: -2, y: 1, a: [1, 2, 3]},
    {_id: 1, x: -1, y: 1, a: [1, 2, 3]},
    {_id: 2, x: 1, y: 1, a: [1, 2, 3]},
    {_id: 3, x: 2, y: 1, a: [1, 2, 3]}
];

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 splitPoint >= 0.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, nss, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

assert.commandWorked(dbConn[collName].insert(docsToInsert));

let listCollRes = assert.commandWorked(dbConn.runCommand({listCollections: 1}));
// There should only be one collection created in this test.
const usingClusteredIndex = listCollRes.cursor.firstBatch[0].options.clusteredIndex != null;

let testCases = [
    {
        logMessage: "Running explain for findAndModify update with sort.",
        hasSort: true,
        opType: "UPDATE",
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 1},
                sort: {x: 1},
                update: {$inc: {z: 1}},
            }
        },
    },
    {
        logMessage: "Running explain for findAndModify update with sort and upsert: true.",
        hasSort: true,
        isUpsert: true,
        opType: "UPDATE",
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 5},  // Query matches no documents.
                sort: {x: 1},
                update: {$inc: {z: 1}},
                upsert: true
            }
        },
    },
    {
        logMessage: "Running explain for findAndModify update.",
        opType: "UPDATE",
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 1},
                update: {$inc: {z: 1}},
            }
        },
    },
    {
        logMessage: "Running explain for findAndModify update without sort and upsert: true.",
        isUpsert: true,
        opType: "UPDATE",
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 5},  // Query matches no documents.
                update: {$inc: {z: 1}},
                upsert: true,
            }
        },
    },
    {
        logMessage: "Running explain for findAndModify remove with sort.",
        hasSort: true,
        opType: "DELETE",
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 1},
                sort: {x: 1},
                remove: true,
            }
        },
    },
    {
        logMessage: "Running explain for findAndModify remove without sort.",
        opType: "DELETE",
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 1},
                remove: true,
            }
        },
    },
    {
        logMessage:
            "Running explain for findAndModify remove with positional projection with sort.",
        opType: "DELETE",
        hasSort: true,
        isPositionalProjection: true,
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 1, a: 1},
                fields: {'a.$': 1},
                sort: {x: 1},
                remove: true,
            }
        }
    },
    {
        logMessage:
            "Running explain for findAndModify remove with positional projection without sort.",
        opType: "DELETE",
        isPositionalProjection: true,
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 1, a: 1},
                fields: {'a.$': 1},
                remove: true,
            }
        }
    },
    {
        logMessage:
            "Running explain for findAndModify update with positional projection with sort.",
        opType: "UPDATE",
        hasSort: true,
        isPositionalProjection: true,
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 1, a: 1},
                sort: {x: 1},
                fields: {'a.$': 1},
                update: {$inc: {z: 1}},
            }
        }
    },
    {
        logMessage:
            "Running explain for findAndModify update with positional projection without sort.",
        opType: "UPDATE",
        isPositionalProjection: true,
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 1, a: 1},
                fields: {'a.$': 1},
                update: {$inc: {z: 1}},
            }
        }
    },
    {
        logMessage: "Running explain for findAndModify update with positional update with sort.",
        opType: "UPDATE",
        hasSort: true,
        isPositionalUpdate: true,
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 1, a: 1},
                sort: {x: 1},
                update: {$set: {"a.$": 3}},
            }
        }
    },
    {
        logMessage: "Running explain for findAndModify update with positional update without sort.",
        opType: "UPDATE",
        isPositionalUpdate: true,
        cmdObj: {
            explain: {
                findAndModify: collName,
                query: {y: 1, a: 1},
                update: {$set: {"a.$": 3}},
            }
        }
    },
    {
        logMessage: "Running explain for updateOne.",
        opType: "UPDATE",
        cmdObj: {
            explain: {
                update: collName,
                updates: [{
                    q: {y: 1},
                    u: {$set: {z: 1}},
                    multi: false,
                    upsert: false,
                }],
            },
        },
    },
    {
        logMessage: "Running explain for updateOne and upsert: true.",
        isUpsert: true,
        opType: "UPDATE",
        cmdObj: {
            explain: {
                update: collName,
                updates: [{
                    q: {y: 5},
                    u: {$set: {z: 1}},
                    multi: false,
                    upsert: true,
                }],  // Query matches no documents.
            },
        },
    },
    {
        logMessage: "Running explain for updateOne and with positional update.",
        opType: "UPDATE",
        isPositionalUpdate: true,
        cmdObj: {
            explain: {
                update: collName,
                updates: [{
                    q: {y: 1, a: 1},
                    u: {$set: {"a.$": 3}},
                    multi: false,
                    upsert: false,
                }],
            },
        },
    },
    {
        logMessage: "Running explain for deleteOne.",
        opType: "DELETE",
        cmdObj: {
            explain: {
                delete: collName,
                deletes: [{q: {y: 1}, limit: 1}],
            },
        },
    },
];

function runTestCase(testCase) {
    jsTestLog(testCase.logMessage + "\n" + tojson(testCase));

    let verbosityLevels = ["queryPlanner", "executionStats", "allPlansExecution"];
    verbosityLevels.forEach(verbosityLevel => {
        jsTestLog("Running with verbosity level: " + verbosityLevel);
        let explainCmdObj = Object.assign(testCase.cmdObj, {verbosity: verbosityLevel});
        let res = assert.commandWorked(dbConn.runCommand(explainCmdObj));
        validateResponse(res, testCase, verbosityLevel);
    });
}

function validateResponse(res, testCase, verbosity) {
    assert.eq(res.queryPlanner.winningPlan.stage, "SHARD_WRITE");

    if (testCase.hasSort) {
        assert.eq(res.queryPlanner.winningPlan.inputStage.winningPlan.stage, "SHARD_MERGE_SORT");
    } else {
        assert.eq(res.queryPlanner.winningPlan.inputStage.winningPlan.stage, "SHARD_MERGE");
    }

    if (testCase.isPositionalProjection) {
        assert.eq(res.queryPlanner.winningPlan.shards[0].winningPlan.stage, "PROJECTION_DEFAULT");
        assert.eq(res.queryPlanner.winningPlan.shards[0].winningPlan.inputStage.stage,
                  testCase.opType);
        if (testCase.hasSort) {
            assert.eq(res.queryPlanner.winningPlan.shards[0]
                          .winningPlan.inputStage.inputStage.inputStage.stage,
                      "FETCH");
            assert.eq(res.queryPlanner.winningPlan.shards[0]
                          .winningPlan.inputStage.inputStage.inputStage.inputStage.stage,
                      "IXSCAN");
        } else {
            if (usingClusteredIndex) {
                assert.eq(
                    res.queryPlanner.winningPlan.shards[0].winningPlan.inputStage.inputStage.stage,
                    "CLUSTERED_IXSCAN");
            } else {
                assert.eq(
                    res.queryPlanner.winningPlan.shards[0].winningPlan.inputStage.inputStage.stage,
                    "FETCH",
                    res);
                assert.eq(res.queryPlanner.winningPlan.shards[0]
                              .winningPlan.inputStage.inputStage.inputStage.stage,
                          "IXSCAN");
            }
        }
    } else if (testCase.isPositionalUpdate) {
        assert.eq(res.queryPlanner.winningPlan.shards[0].winningPlan.stage, testCase.opType);
        if (testCase.hasSort) {
            assert.eq(
                res.queryPlanner.winningPlan.shards[0].winningPlan.inputStage.inputStage.stage,
                "FETCH");
            assert.eq(res.queryPlanner.winningPlan.shards[0]
                          .winningPlan.inputStage.inputStage.inputStage.stage,
                      "IXSCAN");
        } else {
            if (usingClusteredIndex) {
                assert.eq(res.queryPlanner.winningPlan.shards[0].winningPlan.inputStage.stage,
                          "CLUSTERED_IXSCAN");
            } else {
                assert.eq(res.queryPlanner.winningPlan.shards[0].winningPlan.inputStage.stage,
                          "FETCH");
                assert.eq(
                    res.queryPlanner.winningPlan.shards[0].winningPlan.inputStage.inputStage.stage,
                    "IXSCAN");
            }
        }
    } else {
        if (usingClusteredIndex) {
            assert.eq(res.queryPlanner.winningPlan.shards[0].winningPlan.inputStage.stage,
                      "CLUSTERED_IXSCAN");
        } else {
            assert.eq(res.queryPlanner.winningPlan.shards[0].winningPlan.stage, testCase.opType);
            assert.eq(res.queryPlanner.winningPlan.shards[0].winningPlan.inputStage.stage,
                      "IDHACK");
        }
    }

    assert.eq(res.queryPlanner.winningPlan.shards.length,
              1);  // Only 1 shard targeted by the write.
    assert.eq(res.queryPlanner.winningPlan.inputStage.winningPlan.shards.length,
              2);  // 2 shards had matching documents.

    if (verbosity === "queryPlanner") {
        assert.eq(res.executionStats, null);
    } else {
        assert.eq(res.executionStats.executionStages.stage, "SHARD_WRITE");
        if (testCase.isPositionalProjection) {
            assert.eq(res.executionStats.executionStages.shards[0].executionStages.stage,
                      "PROJECTION_DEFAULT");
            assert.eq(res.executionStats.executionStages.shards[0].executionStages.inputStage.stage,
                      testCase.opType);
            if (testCase.hasSort) {
                assert.eq(res.executionStats.executionStages.shards[0]
                              .executionStages.inputStage.inputStage.inputStage.stage,
                          "FETCH");
                assert.eq(res.executionStats.executionStages.shards[0]
                              .executionStages.inputStage.inputStage.inputStage.inputStage.stage,
                          "IXSCAN");
            } else {
                if (usingClusteredIndex) {
                    assert.eq(res.executionStats.executionStages.shards[0]
                                  .executionStages.inputStage.inputStage.stage,
                              "CLUSTERED_IXSCAN");
                } else {
                    assert.eq(res.executionStats.executionStages.shards[0]
                                  .executionStages.inputStage.inputStage.stage,
                              "FETCH");
                    assert.eq(res.executionStats.executionStages.shards[0]
                                  .executionStages.inputStage.inputStage.inputStage.stage,
                              "IXSCAN");
                }
            }
        } else if (testCase.isPositionalUpdate) {
            assert.eq(res.executionStats.executionStages.shards[0].executionStages.stage,
                      testCase.opType);
            if (testCase.hasSort) {
                assert.eq(res.executionStats.executionStages.shards[0]
                              .executionStages.inputStage.inputStage.stage,
                          "FETCH");
                assert.eq(res.executionStats.executionStages.shards[0]
                              .executionStages.inputStage.inputStage.inputStage.stage,
                          "IXSCAN");
            } else {
                if (usingClusteredIndex) {
                    assert.eq(res.executionStats.executionStages.shards[0]
                                  .executionStages.inputStage.stage,
                              "CLUSTERED_IXSCAN");
                } else {
                    assert.eq(res.executionStats.executionStages.shards[0]
                                  .executionStages.inputStage.stage,
                              "FETCH");
                    assert.eq(res.executionStats.executionStages.shards[0]
                                  .executionStages.inputStage.inputStage.stage,
                              "IXSCAN");
                }
            }
        } else {
            if (usingClusteredIndex) {
                assert.eq(
                    res.executionStats.executionStages.shards[0].executionStages.inputStage.stage,
                    "CLUSTERED_IXSCAN");

            } else {
                assert.eq(res.executionStats.executionStages.shards[0].executionStages.stage,
                          testCase.opType);
                assert.eq(
                    res.executionStats.executionStages.shards[0].executionStages.inputStage.stage,
                    "IDHACK");
            }
        }
        assert.eq(res.executionStats.executionStages.shards.length,
                  1);  // Only 1 shard targeted by the write.
        assert.eq(res.executionStats.inputStage.executionStages.shards.length,
                  2);  // 2 shards had matching documents.

        // We use a dummy _id target document for the Write Phase which should not match any
        // existing documents in the collection. This will at least preserve the query plan,
        // but may lead to incorrect executionStats.
        if (testCase.isUpsert) {
            assert.eq(res.executionStats.nReturned, 0);
            assert.eq(res.executionStats.executionStages.shards[0].executionStages.nWouldModify, 0);
            assert.eq(res.executionStats.executionStages.shards[0].executionStages.nWouldUpsert, 1);
            assert.eq(res.executionStats.inputStage.nReturned, 0);
        } else {
            // TODO SERVER-29449: Properly report explain results for sharded queries with a
            // limit. assert.eq(res.executionStats.nReturned, 1);
            if (testCase.opType === "DELETE") {
                // We use a dummy _id target document for the Write Phase which should not match any
                // existing documents in the collection. This will at least preserve the query plan,
                // but may lead to incorrect executionStats.
                if (testCase.isPositionalProjection) {
                    assert.eq(res.executionStats.executionStages.shards[0]
                                  .executionStages.inputStage.nWouldDelete,
                              0);
                } else {
                    assert.eq(
                        res.executionStats.executionStages.shards[0].executionStages.nWouldDelete,
                        0);
                }
            } else {
                // We use a dummy _id target document for the Write Phase which should not match any
                // existing documents in the collection. This will at least preserve the query plan,
                // but may lead to incorrect executionStats.
                if (testCase.isPositionalProjection) {
                    assert.eq(res.executionStats.executionStages.shards[0]
                                  .executionStages.inputStage.nWouldModify,
                              0);
                    assert.eq(res.executionStats.executionStages.shards[0]
                                  .executionStages.inputStage.nWouldUpsert,
                              0);
                } else {
                    assert.eq(
                        res.executionStats.executionStages.shards[0].executionStages.nWouldModify,
                        0);
                    assert.eq(
                        res.executionStats.executionStages.shards[0].executionStages.nWouldUpsert,
                        0);
                }
            }
            assert.eq(res.executionStats.inputStage.nReturned, 2);
        }

        if (testCase.hasSort) {
            assert.eq(res.executionStats.inputStage.executionStages.stage, "SHARD_MERGE_SORT");
        } else {
            assert.eq(res.executionStats.inputStage.executionStages.stage, "SHARD_MERGE");
        }
    }

    assert(res.serverInfo);
    assert(res.serverParameters);
    assert(res.command);

    // Checks that 'command' field of the explain output is the same command that we originally
    // wanted to explain.
    for (const [key, value] of Object.entries(testCase.cmdObj.explain)) {
        assert.eq(res.command[key], value);
    }
}

testCases.forEach(testCase => {
    runTestCase(testCase);
});

st.stop();
})();
