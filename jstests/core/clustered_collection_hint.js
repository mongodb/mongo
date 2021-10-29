/**
 * Tests that a collection with a clustered index can use and interpret a query hint.
 * @tags: [
 *   requires_fcv_52,
 *   // Does not support sharding
 *   assumes_against_mongod_not_mongos,
 *   assumes_unsharded_collection,
 * ]
 */
(function() {
"use strict";
load("jstests/libs/analyze_plan.js");
load("jstests/libs/collection_drop_recreate.js");

const clusteredIndexesEnabled = assert
                                    .commandWorked(db.getMongo().adminCommand(
                                        {getParameter: 1, featureFlagClusteredIndexes: 1}))
                                    .featureFlagClusteredIndexes.value;

if (!clusteredIndexesEnabled) {
    jsTestLog('Skipping test because the clustered indexes feature flag is disabled');
    return;
}

const testDB = db.getSiblingDB(jsTestName());
const collName = "coll";
const coll = testDB[collName];
assertDropCollection(testDB, collName);

const validateHint = ({expectedNReturned, cmd, expectedWinningPlanStats = {}}) => {
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
};

assert.commandWorked(
    testDB.createCollection(collName, {clusteredIndex: {key: {_id: 1}, unique: true}}));

// Create an index that the query planner would consider preferable to using the cluster key for
// point predicates on 'a'.
const idxA = {
    a: -1
};
assert.commandWorked(coll.createIndex(idxA));

const batchSize = 100;
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < batchSize; i++) {
    bulk.insert({_id: i, a: -i});
}
assert.commandWorked(bulk.execute());
assert.eq(coll.find().itcount(), batchSize);

// Basic find with hints on cluster key.
validateHint({
    expectedNReturned: batchSize,
    cmd: {
        find: collName,
        hint: {_id: 1},
    },
    expectedWinningPlanStats: {
        stage: "COLLSCAN",
        direction: "forward",
    }
});
validateHint({
    expectedNReturned: batchSize,
    cmd: {
        find: collName,
        hint: "_id_",
    },
    expectedWinningPlanStats: {
        stage: "COLLSCAN",
        direction: "forward",
    }
});
validateHint({
    expectedNReturned: 1,
    cmd: {
        find: collName,
        filter: {a: -2},
        hint: {_id: 1},
    },
    expectedWinningPlanStats: {
        stage: "COLLSCAN",
        direction: "forward",
    }
});
validateHint({
    expectedNReturned: 1,
    cmd: {
        find: collName,
        filter: {a: -2},
        hint: "_id_",
    },
    expectedWinningPlanStats: {
        stage: "COLLSCAN",
        direction: "forward",
    }
});

// Find with hints on cluster key that generate bounded collection scans.
const arbitraryDocId = 12;
validateHint({
    expectedNReturned: 1,
    cmd: {
        find: collName,
        filter: {_id: arbitraryDocId},
        hint: {_id: 1},
    },
    expectedWinningPlanStats: {
        stage: "COLLSCAN",
        direction: "forward",
        minRecord: arbitraryDocId,
        maxRecord: arbitraryDocId
    }
});
validateHint({
    expectedNReturned: 1,
    cmd: {
        find: collName,
        filter: {_id: arbitraryDocId},
        hint: "_id_",
    },
    expectedWinningPlanStats: {
        stage: "COLLSCAN",
        direction: "forward",
        minRecord: arbitraryDocId,
        maxRecord: arbitraryDocId
    }
});
validateHint({
    expectedNReturned: arbitraryDocId,
    cmd: {
        find: collName,
        filter: {_id: {$lt: arbitraryDocId}},
        hint: {_id: 1},
    },
    expectedWinningPlanStats: {stage: "COLLSCAN", direction: "forward", maxRecord: arbitraryDocId}
});
validateHint({
    expectedNReturned: batchSize - arbitraryDocId,
    cmd: {
        find: collName,
        filter: {_id: {$gte: arbitraryDocId}},
        hint: {_id: 1},
    },
    expectedWinningPlanStats: {stage: "COLLSCAN", direction: "forward", minRecord: arbitraryDocId}
});

// Find with $natural hints.
validateHint({
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
validateHint({
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
validateHint({
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
validateHint({
    expectedNReturned: batchSize,
    cmd: {find: collName, hint: idxA},
    expectedWinningPlanStats: {
        stage: "IXSCAN",
        keyPattern: idxA,
    }
});

// Update with hint on cluster key.
validateHint({
    expectedNReturned: 0,
    cmd: {update: collName, updates: [{q: {_id: 3}, u: {$inc: {a: -2}}, hint: {_id: 1}}]},
    expectedWinningPlanStats: {
        stage: "COLLSCAN",
        direction: "forward",
    }
});

// Update with reverse $natural hint.
validateHint({
    expectedNReturned: 0,
    cmd: {update: collName, updates: [{q: {_id: 80}, u: {$inc: {a: 80}}, hint: {$natural: -1}}]},
    expectedWinningPlanStats: {
        stage: "COLLSCAN",
        direction: "backward",
    }
});

// Update with hint on secondary index.
validateHint({
    expectedNReturned: 0,
    cmd: {update: collName, updates: [{q: {a: -2}, u: {$set: {a: 2}}, hint: idxA}]},
    expectedWinningPlanStats: {
        stage: "IXSCAN",
        keyPattern: idxA,
    }
});

// Delete with hint on cluster key.
validateHint({
    expectedNReturned: 0,
    cmd: {delete: collName, deletes: [{q: {_id: 2}, limit: 0, hint: {_id: 1}}]},
    expectedWinningPlanStats: {
        stage: "COLLSCAN",
        direction: "forward",
    }
});

// Delete reverse $natural hint.
validateHint({
    expectedNReturned: 0,
    cmd: {delete: collName, deletes: [{q: {_id: 30}, limit: 0, hint: {$natural: -1}}]},
    expectedWinningPlanStats: {
        stage: "COLLSCAN",
        direction: "backward",
    }
});

// Delete with hint on standard index.
validateHint({
    expectedNReturned: 0,
    cmd: {delete: collName, deletes: [{q: {a: -5}, limit: 0, hint: idxA}]},
    expectedWinningPlanStats: {
        stage: "IXSCAN",
        keyPattern: idxA,
    }
});

// Reverse 'hint' on the cluster key is illegal.
assert.commandFailedWithCode(testDB.runCommand({find: collName, hint: {_id: -1}}),
                             ErrorCodes.BadValue);
})();
