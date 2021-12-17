/**
 * Validate $hint on a clustered collection.
 */

const testClusteredCollectionHint = function(coll, clusterKey, clusterKeyName) {
    "use strict";
    load("jstests/libs/analyze_plan.js");
    load("jstests/libs/collection_drop_recreate.js");

    const clusterKeyFieldName = Object.keys(clusterKey)[0];
    const batchSize = 100;

    function validateHint(coll, {expectedNReturned, cmd, expectedWinningPlanStats = {}}) {
        const explain = assert.commandWorked(coll.runCommand({explain: cmd}));
        assert.eq(explain.executionStats.nReturned, expectedNReturned, tojson(explain));

        const actualWinningPlan = getWinningPlan(explain.queryPlanner);
        const stageOfInterest = getPlanStage(actualWinningPlan, expectedWinningPlanStats.stage);
        assert.neq(null, stageOfInterest);

        for (const [key, value] of Object.entries(expectedWinningPlanStats)) {
            assert(stageOfInterest[key], tojson(explain));
            assert.eq(stageOfInterest[key], value, tojson(explain));
        }

        // Explicitly check that the plan is not bounded by default.
        if (!expectedWinningPlanStats.hasOwnProperty("minRecord")) {
            assert(!actualWinningPlan["minRecord"], tojson(explain));
        }
        if (!expectedWinningPlanStats.hasOwnProperty("maxRecord")) {
            assert(!actualWinningPlan["maxRecord"], tojson(explain));
        }
    }

    function testHint(coll, clusterKey, clusterKeyName) {
        // Create clustered collection.
        assertDropCollection(coll.getDB(), coll.getName());
        assert.commandWorked(coll.getDB().createCollection(
            coll.getName(), {clusteredIndex: {key: {[clusterKeyFieldName]: 1}, unique: true}}));

        // Create an index that the query planner would consider preferable to using the cluster key
        // for point predicates on 'a'.
        const idxA = {a: -1};
        assert.commandWorked(coll.createIndex(idxA));

        // Populate collection.
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < batchSize; i++) {
            bulk.insert({[clusterKeyFieldName]: i, a: -i});
        }
        assert.commandWorked(bulk.execute());
        assert.eq(coll.find().itcount(), batchSize);

        const collName = coll.getName();

        // Basic find with hints on cluster key.
        validateHint(coll, {
            expectedNReturned: batchSize,
            cmd: {
                find: collName,
                hint: clusterKey,
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "forward",
            }
        });
        validateHint(coll, {
            expectedNReturned: batchSize,
            cmd: {
                find: collName,
                hint: clusterKeyName,
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "forward",
            }
        });
        validateHint(coll, {
            expectedNReturned: 1,
            cmd: {
                find: collName,
                filter: {a: -2},
                hint: clusterKey,
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "forward",
            }
        });
        validateHint(coll, {
            expectedNReturned: 1,
            cmd: {
                find: collName,
                filter: {a: -2},
                hint: clusterKeyName,
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "forward",
            }
        });

        // Find with hints on cluster key that generate bounded collection scans.
        const arbitraryDocId = 12;
        validateHint(coll, {
            expectedNReturned: 1,
            cmd: {
                find: collName,
                filter: {[clusterKeyFieldName]: arbitraryDocId},
                hint: clusterKey,
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "forward",
                minRecord: arbitraryDocId,
                maxRecord: arbitraryDocId
            }
        });
        validateHint(coll, {
            expectedNReturned: 0,
            cmd: {
                find: collName,
                hint: clusterKey,
                min: {[clusterKeyFieldName]: 101},
                max: {[clusterKeyFieldName]: MaxKey}
            },
            expectedWinningPlanStats:
                {stage: "COLLSCAN", direction: "forward", minRecord: 101, maxRecord: MaxKey}
        });
        validateHint(coll, {
            expectedNReturned: 0,
            cmd: {
                find: collName,
                hint: clusterKey,
                min: {[clusterKeyFieldName]: MinKey},
                max: {[clusterKeyFieldName]: -2}
            },
            expectedWinningPlanStats:
                {stage: "COLLSCAN", direction: "forward", minRecord: MinKey, maxRecord: -2}
        });
        validateHint(coll, {
            expectedNReturned: 1,
            cmd: {
                find: collName,
                filter: {[clusterKeyFieldName]: arbitraryDocId},
                hint: clusterKeyName,
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "forward",
                minRecord: arbitraryDocId,
                maxRecord: arbitraryDocId
            }
        });
        validateHint(coll, {
            expectedNReturned: arbitraryDocId,
            cmd: {
                find: collName,
                filter: {[clusterKeyFieldName]: {$lt: arbitraryDocId}},
                hint: clusterKey,
            },
            expectedWinningPlanStats:
                {stage: "COLLSCAN", direction: "forward", maxRecord: arbitraryDocId}
        });
        validateHint(coll, {
            expectedNReturned: batchSize - arbitraryDocId,
            cmd: {
                find: collName,
                filter: {[clusterKeyFieldName]: {$gte: arbitraryDocId}},
                hint: clusterKey,
            },
            expectedWinningPlanStats:
                {stage: "COLLSCAN", direction: "forward", minRecord: arbitraryDocId}
        });

        // Find with $natural hints.
        validateHint(coll, {
            expectedNReturned: batchSize,
            cmd: {
                find: collName,
                hint: {$natural: -1},
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "backward",
            }
        });
        validateHint(coll, {
            expectedNReturned: batchSize,
            cmd: {
                find: collName,
                hint: {$natural: 1},
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "forward",
            }
        });
        validateHint(coll, {
            expectedNReturned: 1,
            cmd: {
                find: collName,
                filter: {a: -2},
                hint: {$natural: -1},
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "backward",
            }
        });

        // Find on a standard index.
        validateHint(coll, {
            expectedNReturned: batchSize,
            cmd: {find: collName, hint: idxA},
            expectedWinningPlanStats: {
                stage: "IXSCAN",
                keyPattern: idxA,
            }
        });

        // Update with hint on cluster key.
        validateHint(coll, {
            expectedNReturned: 0,
            cmd: {
                update: collName,
                updates: [{q: {[clusterKeyFieldName]: 3}, u: {$inc: {a: -2}}, hint: clusterKey}]
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "forward",
            }
        });

        // Update with reverse $natural hint.
        validateHint(coll, {
            expectedNReturned: 0,
            cmd: {
                update: collName,
                updates:
                    [{q: {[clusterKeyFieldName]: 80}, u: {$inc: {a: 80}}, hint: {$natural: -1}}]
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "backward",
            }
        });

        // Update with hint on secondary index.
        validateHint(coll, {
            expectedNReturned: 0,
            cmd: {update: collName, updates: [{q: {a: -2}, u: {$set: {a: 2}}, hint: idxA}]},
            expectedWinningPlanStats: {
                stage: "IXSCAN",
                keyPattern: idxA,
            }
        });

        // Delete with hint on cluster key.
        validateHint(coll, {
            expectedNReturned: 0,
            cmd: {
                delete: collName,
                deletes: [{q: {[clusterKeyFieldName]: 2}, limit: 0, hint: clusterKey}]
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "forward",
            }
        });

        // Delete reverse $natural hint.
        validateHint(coll, {
            expectedNReturned: 0,
            cmd: {
                delete: collName,
                deletes: [{q: {[clusterKeyFieldName]: 30}, limit: 0, hint: {$natural: -1}}]
            },
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "backward",
            }
        });

        // Delete with hint on standard index.
        validateHint(coll, {
            expectedNReturned: 0,
            cmd: {delete: collName, deletes: [{q: {a: -5}, limit: 0, hint: idxA}]},
            expectedWinningPlanStats: {
                stage: "IXSCAN",
                keyPattern: idxA,
            }
        });

        // Reverse 'hint' on the cluster key is illegal.
        assert.commandFailedWithCode(
            coll.getDB().runCommand({find: coll.getName(), hint: {[clusterKeyFieldName]: -1}}),
            ErrorCodes.BadValue);
    }

    return testHint(coll, clusterKey, clusterKeyName);
};
