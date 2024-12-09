/**
 * Shared test cases and collection setup for tests checking explain and slow query log optimization
 * stats.
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

export function runWithFailpoint(db, failpointName, failpointOpts, func) {
    let failPoints = [];
    try {
        failPoints = FixtureHelpers.mapOnEachShardNode({
            db: db.getSiblingDB("admin"),
            func: (db) => configureFailPoint(db, failpointName, failpointOpts),
            primaryNodeOnly: true,
        });

        func();
    } finally {
        failPoints.forEach(failPoint => failPoint.off());
    }
}

/**
 * Returns an array of test cases, where each has the format:
 * {testName: "...", command: {...}, failpointName: "...", failpointOpts: {...}}.
 * All commands are explain queries.
 */
export function setupCollectionAndGetExplainTestCases(db, collName, waitTimeMillis) {
    const filter = {a: "abc", b: "def", c: {$gt: 50}};

    assertDropAndRecreateCollection(db, collName);

    const collection = db[collName];

    assert.commandWorked(collection.createIndex({a: 1}));
    assert.commandWorked(collection.createIndex({b: 1}));
    assert.commandWorked(
        collection.insertMany(Array.from({length: 100}, (_, i) => ({a: "abc", b: "def", c: i}))));

    assert.commandWorked(db.createView("view", collName, [{$match: filter}]));

    // These commands will use multiplanning.
    const commands = [
        {testName: "find", command: {explain: {find: collName, filter: filter}}},
        {testName: "count", command: {explain: {count: collName, query: filter}}},
        {testName: "distinct", command: {explain: {distinct: collName, key: "c", query: filter}}},
        {
            testName: "findAndModify",
            command: {explain: {findAndModify: collName, query: filter, update: {$inc: {c: 1}}}}
        },
        {
            testName: "findAndModify upsert",
            command: {
                explain: {
                    findAndModify: collName,
                    query: filter,
                    update: {$inc: {c: 1}},
                    upsert: true
                }
            }
        },
        {
            testName: "delete",
            command: {explain: {delete: collName, deletes: [{q: filter, limit: 0}]}}
        },
        {
            testName: "update",
            command: {explain: {update: collName, updates: [{q: filter, u: {$inc: {c: 1}}}]}}
        },
        {
            testName: "bulkWrite",
            command: {
                explain: {
                    bulkWrite: 1,
                    ops: [{update: 0, filter: filter, updateMods: {$inc: {c: 1}}}],
                    nsInfo: [{ns: collection.getFullName()}],
                }
            },
        },
        {
            testName: "aggregate with explain command",
            command: {explain: {aggregate: collName, pipeline: [{$match: filter}], cursor: {}}}
        },
        {
            testName: "aggregate with explain flag",
            command: {aggregate: collName, pipeline: [{$match: filter}], cursor: {}, explain: true}
        },
        {
            testName: "aggregate with subpipeline",
            command: {
                explain: {
                    aggregate: collName,
                    pipeline: [
                        {$match: filter},
                        {
                            $lookup: {
                                from: collName,
                                as: "arr",
                                let: {local_c: "$c"},
                                pipeline: [{$match: {$expr: {$gte: ["$c", "$$local_c"]}}}]
                            }
                        }
                    ],
                    cursor: {}
                }
            }
        },
        {
            testName: "aggregate on view",
            command: {explain: {aggregate: "view", pipeline: [], cursor: {}}}
        },
        {
            testName: "aggregate with getMore",
            command: {explain: {aggregate: collName, pipeline: [{$match: filter}],
            cursor: {batchSize: 2}}}
        },
        {
            testName: "mapReduce",
            command: {
                explain: {
                    mapReduce: collName,
                    query: filter,
                    map: function() {
                        emit("val", 1);
                    },
                    reduce: function(k, v) {
                        return 1;
                    },
                    out: "example"
                }
            }
        },
    ];

    // These commands will not be multiplanned, so we need a different failpoint to test them.
    const nonMultiplanningCommands = [
        {
            testName: "express-eligible find-by-id",
            command: {explain: {find: collName, filter: {_id: 0}}}
        },
        {
            testName: "express-eligible single-field find",
            command: {explain: {find: collName, filter: {a: "abc"}, limit: 1}}
        },
        {
            testName: "single-field find that doesn't multiplan",
            command: {explain: {find: collName, filter: {a: "abc"}}}
        },
    ];

    let failpointBase = {
        failpointName: "sleepWhileMultiplanning",
        failpointOpts: {ms: waitTimeMillis}
    };
    const testCases = commands.map(cmd => Object.assign({}, failpointBase, cmd));

    failpointBase = {
        failpointName: "setAutoGetCollectionWait",
        failpointOpts: {waitForMillis: waitTimeMillis}
    };
    const nonMultiPlanningTestCases =
        nonMultiplanningCommands.map(cmd => Object.assign({}, failpointBase, cmd));

    return testCases.concat(nonMultiPlanningTestCases);
}
