/**
 * Validate $hint on a clustered collection.
 */
import {getPlanStage, getWinningPlan} from "jstests/libs/analyze_plan.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

export function testClusteredCollectionHint(coll, clusterKey, clusterKeyName) {
    const clusterKeyFieldName = Object.keys(clusterKey)[0];
    const batchSize = 100;

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
        validateClusteredCollectionHint(coll, {
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
        validateClusteredCollectionHint(coll, {
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
        validateClusteredCollectionHint(coll, {
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
        validateClusteredCollectionHint(coll, {
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
        validateClusteredCollectionHint(coll, {
            expectedNReturned: 1,
            cmd: {
                find: collName,
                filter: {[clusterKeyFieldName]: arbitraryDocId},
                hint: clusterKey,
            },
            expectedWinningPlanStats: {
                stage: "CLUSTERED_IXSCAN",
                direction: "forward",
                minRecord: arbitraryDocId,
                maxRecord: arbitraryDocId
            }
        });
        validateClusteredCollectionHint(coll, {
            expectedNReturned: 0,
            cmd: {
                find: collName,
                hint: clusterKey,
                min: {[clusterKeyFieldName]: 101},
                max: {[clusterKeyFieldName]: MaxKey}
            },
            expectedWinningPlanStats:
                {stage: "CLUSTERED_IXSCAN", direction: "forward", minRecord: 101, maxRecord: MaxKey}
        });
        validateClusteredCollectionHint(coll, {
            expectedNReturned: 0,
            cmd: {
                find: collName,
                hint: clusterKey,
                min: {[clusterKeyFieldName]: MinKey},
                max: {[clusterKeyFieldName]: -2}
            },
            expectedWinningPlanStats:
                {stage: "CLUSTERED_IXSCAN", direction: "forward", minRecord: MinKey, maxRecord: -2}
        });
        validateClusteredCollectionHint(coll, {
            expectedNReturned: 1,
            cmd: {
                find: collName,
                filter: {[clusterKeyFieldName]: arbitraryDocId},
                hint: clusterKeyName,
            },
            expectedWinningPlanStats: {
                stage: "CLUSTERED_IXSCAN",
                direction: "forward",
                minRecord: arbitraryDocId,
                maxRecord: arbitraryDocId
            }
        });
        validateClusteredCollectionHint(coll, {
            expectedNReturned: arbitraryDocId,
            cmd: {
                find: collName,
                filter: {[clusterKeyFieldName]: {$lt: arbitraryDocId}},
                hint: clusterKey,
            },
            expectedWinningPlanStats: {
                stage: "CLUSTERED_IXSCAN",
                direction: "forward",
                minRecord: NaN,
                maxRecord: arbitraryDocId
            }
        });
        validateClusteredCollectionHint(coll, {
            expectedNReturned: batchSize - arbitraryDocId,
            cmd: {
                find: collName,
                filter: {[clusterKeyFieldName]: {$gte: arbitraryDocId}},
                hint: clusterKey,
            },
            expectedWinningPlanStats: {
                stage: "CLUSTERED_IXSCAN",
                direction: "forward",
                minRecord: arbitraryDocId,
                maxRecord: Infinity
            }
        });

        // Find with $natural hints.
        validateClusteredCollectionHint(coll, {
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
        validateClusteredCollectionHint(coll, {
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
        validateClusteredCollectionHint(coll, {
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

        // Find with $natural hints and sorts: we should scan the collection in the hinted
        // direction regardless of sort direction, and provide a blocking sort if needed.
        validateClusteredCollectionHint(coll, {
            expectedNReturned: batchSize,
            cmd: {find: collName, hint: {$natural: 1}, sort: {[clusterKeyFieldName]: 1}},
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "forward",
            },
            unexpectedWinningPlanStats: ["SORT"]  // We shouldn't need a blocking sort here.
        });
        validateClusteredCollectionHint(coll, {
            expectedNReturned: batchSize,
            cmd: {find: collName, hint: {$natural: -1}, sort: {[clusterKeyFieldName]: 1}},
            expectedWinningPlanStats: [
                {stage: "SORT", sortPattern: {[clusterKeyFieldName]: 1}},
                {
                    stage: "COLLSCAN",
                    direction: "backward",
                }
            ]
        });
        validateClusteredCollectionHint(coll, {
            expectedNReturned: batchSize,
            cmd: {find: collName, hint: {$natural: 1}, sort: {[clusterKeyFieldName]: -1}},
            expectedWinningPlanStats: [
                {stage: "SORT", sortPattern: {[clusterKeyFieldName]: -1}},
                {
                    stage: "COLLSCAN",
                    direction: "forward",
                }
            ]
        });
        validateClusteredCollectionHint(coll, {
            expectedNReturned: batchSize,
            cmd: {find: collName, hint: {$natural: -1}, sort: {[clusterKeyFieldName]: -1}},
            expectedWinningPlanStats: {
                stage: "COLLSCAN",
                direction: "backward",
            },
            unexpectedWinningPlanStats: ["SORT"]  // We shouldn't need a blocking sort here.
        });

        // We always need a blocking sort when the sort pattern does not match the provided sort for
        // the clustered collection.
        validateClusteredCollectionHint(coll, {
            expectedNReturned: batchSize,
            cmd: {find: collName, hint: {$natural: 1}, sort: {a: 1}},
            expectedWinningPlanStats: [
                {stage: "SORT", sortPattern: {a: 1}},
                {
                    stage: "COLLSCAN",
                    direction: "forward",
                }
            ]
        });
        validateClusteredCollectionHint(coll, {
            expectedNReturned: batchSize,
            cmd: {find: collName, hint: {$natural: -1}, sort: {a: 1}},
            expectedWinningPlanStats: [
                {stage: "SORT", sortPattern: {a: 1}},
                {
                    stage: "COLLSCAN",
                    direction: "backward",
                }
            ]
        });
        validateClusteredCollectionHint(coll, {
            expectedNReturned: batchSize,
            cmd: {find: collName, hint: {$natural: 1}, sort: {a: -1}},
            expectedWinningPlanStats: [
                {stage: "SORT", sortPattern: {a: -1}},
                {
                    stage: "COLLSCAN",
                    direction: "forward",
                }
            ]
        });
        validateClusteredCollectionHint(coll, {
            expectedNReturned: batchSize,
            cmd: {find: collName, hint: {$natural: -1}, sort: {a: -1}},
            expectedWinningPlanStats:
                [{stage: "SORT", sortPattern: {a: -1}}, {stage: "COLLSCAN", direction: "backward"}],
        });

        // Find on a standard index.
        if (!TestData.isHintsToQuerySettingsSuite) {
            // This guard excludes this test case from being run on the
            // cursor_hints_to_query_settings suite. The suite replaces cursor hints with query
            // settings. Query settings do not force indexes, and therefore empty filter will result
            // in collection scans.
            validateClusteredCollectionHint(coll, {
                expectedNReturned: batchSize,
                cmd: {find: collName, hint: idxA},
                expectedWinningPlanStats: {
                    stage: "IXSCAN",
                    keyPattern: idxA,
                }
            });
        }

        // Update with hint on cluster key.
        validateClusteredCollectionHint(coll, {
            expectedNReturned: 0,
            cmd: {
                update: collName,
                updates: [{q: {[clusterKeyFieldName]: 3}, u: {$inc: {a: -2}}, hint: clusterKey}]
            },
            expectedWinningPlanStats:
                {stage: "CLUSTERED_IXSCAN", direction: "forward", minRecord: 3, maxRecord: 3}
        });

        // Update with reverse $natural hint.
        validateClusteredCollectionHint(coll, {
            expectedNReturned: 0,
            cmd: {
                update: collName,
                updates:
                    [{q: {[clusterKeyFieldName]: 80}, u: {$inc: {a: 80}}, hint: {$natural: -1}}]
            },
            expectedWinningPlanStats:
                {stage: "CLUSTERED_IXSCAN", direction: "backward", minRecord: 80, maxRecord: 80}
        });

        // Update with hint on secondary index.
        validateClusteredCollectionHint(coll, {
            expectedNReturned: 0,
            cmd: {update: collName, updates: [{q: {a: -2}, u: {$set: {a: 2}}, hint: idxA}]},
            expectedWinningPlanStats: {
                stage: "IXSCAN",
                keyPattern: idxA,
            }
        });

        // Delete with hint on cluster key.
        validateClusteredCollectionHint(coll, {
            expectedNReturned: 0,
            cmd: {
                delete: collName,
                deletes: [{q: {[clusterKeyFieldName]: 2}, limit: 0, hint: clusterKey}]
            },
            expectedWinningPlanStats:
                {stage: "CLUSTERED_IXSCAN", direction: "forward", minRecord: 2, maxRecord: 2}
        });

        // Delete reverse $natural hint.
        validateClusteredCollectionHint(coll, {
            expectedNReturned: 0,
            cmd: {
                delete: collName,
                deletes: [{q: {[clusterKeyFieldName]: 30}, limit: 0, hint: {$natural: -1}}]
            },
            expectedWinningPlanStats:
                {stage: "CLUSTERED_IXSCAN", direction: "backward", minRecord: 30, maxRecord: 30}
        });

        // Delete with hint on standard index.
        validateClusteredCollectionHint(coll, {
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
}

export function validateClusteredCollectionHint(
    coll,
    {expectedNReturned, cmd, expectedWinningPlanStats = {}, unexpectedWinningPlanStats = []}) {
    const explain = assert.commandWorked(coll.runCommand({explain: cmd}));
    assert.eq(explain.executionStats.nReturned, expectedNReturned, tojson(explain));

    const actualWinningPlan = getWinningPlan(explain.queryPlanner);

    if (!Array.isArray(expectedWinningPlanStats)) {
        expectedWinningPlanStats = [expectedWinningPlanStats];
    }

    for (const excludedStage of unexpectedWinningPlanStats) {
        const stageOfInterest = getPlanStage(actualWinningPlan, excludedStage);
        assert.eq(null, stageOfInterest);
    }

    for (const expectedWinningPlanStageStats of expectedWinningPlanStats) {
        const stageOfInterest =
            getPlanStage(actualWinningPlan, expectedWinningPlanStageStats.stage);
        assert.neq(null, stageOfInterest);

        for (const [key, value] of Object.entries(expectedWinningPlanStageStats)) {
            assert(stageOfInterest[key] !== undefined, tojson(explain));
            assert.eq(stageOfInterest[key], value, tojson(explain));
        }

        // Explicitly check that the plan is not bounded by default.
        if (!expectedWinningPlanStageStats.hasOwnProperty("minRecord")) {
            assert(!actualWinningPlan.hasOwnProperty("minRecord"), tojson(explain));
        }
        if (!expectedWinningPlanStageStats.hasOwnProperty("maxRecord")) {
            assert(!actualWinningPlan.hasOwnProperty("maxRecord"), tojson(explain));
        }
    }
}
