/**
 * Tests that query options are not dropped by mongos when a query against a view is rewritten as an
 * aggregation against the underlying collection.
 */
import {profilerHasSingleMatchingEntryOrThrow} from "jstests/libs/profiler.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    name: "view_rewrite",
    shards: 2,
    other: {
        rs0: {
            nodes: [{rsConfig: {priority: 1}}, {rsConfig: {priority: 0, tags: {"tag": "secondary"}}}],
        },
        rs1: {
            nodes: [{rsConfig: {priority: 1}}, {rsConfig: {priority: 0, tags: {"tag": "secondary"}}}],
        },
        enableBalancer: false,
    },
});

const mongos = st.s0;
const config = mongos.getDB("config");
const mongosDB = mongos.getDB("view_rewrite");
const coll = mongosDB.getCollection("coll");

assert.commandWorked(config.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.shard0.shardName}));

assert.commandWorked(config.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));
assert.commandWorked(mongos.adminCommand({split: coll.getFullName(), middle: {a: 5}}));
assert.commandWorked(mongosDB.adminCommand({moveChunk: coll.getFullName(), find: {a: 5}, to: st.shard1.shardName}));

for (let i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}

assert.commandWorked(mongosDB.createView("view", coll.getName(), []));

//
// Confirms that queries run against views on mongos result in execution of a rewritten
// aggregation that contains all expected query options.
//
function confirmOptionsInProfiler(shardPrimary) {
    assert.commandWorked(shardPrimary.setProfilingLevel(2));

    // Aggregation
    assert.commandWorked(
        mongosDB.runCommand({
            aggregate: "view",
            pipeline: [],
            comment: "agg_rewrite",
            maxTimeMS: 5 * 60 * 1000,
            readConcern: {level: "linearizable"},
            cursor: {},
        }),
    );

    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shardPrimary,
        filter: {
            "ns": coll.getFullName(),
            "command.aggregate": coll.getName(),
            "command.comment": "agg_rewrite",
            "command.maxTimeMS": {"$exists": true},
            "command.readConcern": {level: "linearizable"},
            "command.pipeline.$mergeCursors": {"$exists": false},
            "nreturned": {"$exists": true},
        },
    });

    // Find
    assert.commandWorked(
        mongosDB.runCommand({
            find: "view",
            comment: "find_rewrite",
            maxTimeMS: 5 * 60 * 1000,
            readConcern: {level: "linearizable"},
        }),
    );

    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shardPrimary,
        filter: {
            "ns": coll.getFullName(),
            "command.aggregate": coll.getName(),
            "command.comment": "find_rewrite",
            "command.maxTimeMS": {"$exists": true},
            "command.readConcern": {level: "linearizable"},
            "command.pipeline.$mergeCursors": {"$exists": false},
            "nreturned": {"$exists": true},
        },
    });

    // Count
    assert.commandWorked(
        mongosDB.runCommand({
            count: "view",
            comment: "count_rewrite",
            maxTimeMS: 5 * 60 * 1000,
            readConcern: {level: "linearizable"},
        }),
    );

    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shardPrimary,
        filter: {
            "ns": coll.getFullName(),
            "command.aggregate": coll.getName(),
            "command.comment": "count_rewrite",
            "command.maxTimeMS": {"$exists": true},
            "command.readConcern": {level: "linearizable"},
            "command.pipeline.$mergeCursors": {"$exists": false},
            "nreturned": {"$exists": true},
        },
    });

    // Distinct
    assert.commandWorked(
        mongosDB.runCommand({
            distinct: "view",
            key: "a",
            comment: "distinct_rewrite",
            maxTimeMS: 5 * 60 * 1000,
            readConcern: {level: "linearizable"},
        }),
    );

    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shardPrimary,
        filter: {
            "ns": coll.getFullName(),
            "command.aggregate": coll.getName(),
            "command.comment": "distinct_rewrite",
            "command.maxTimeMS": {"$exists": true},
            "command.readConcern": {level: "linearizable"},
            "command.pipeline.$mergeCursors": {"$exists": false},
            "nreturned": {"$exists": true},
        },
    });

    assert.commandWorked(shardPrimary.setProfilingLevel(0));
    shardPrimary.system.profile.drop();
}

//
// Confirms that queries run against views on mongos are executed against a tagged secondary, as
// per readPreference setting.
//
function confirmReadPreference(shardSecondary) {
    assert.commandWorked(shardSecondary.setProfilingLevel(2));

    // Aggregation
    assert.commandWorked(
        mongosDB.runCommand({
            aggregate: "view",
            pipeline: [],
            comment: "agg_readPref",
            cursor: {},
            $readPreference: {mode: "nearest", tags: [{tag: "secondary"}]},
            readConcern: {level: "local"},
        }),
    );

    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shardSecondary,
        filter: {
            "ns": coll.getFullName(),
            "command.aggregate": coll.getName(),
            "command.comment": "agg_readPref",
            "command.pipeline.$mergeCursors": {"$exists": false},
            "nreturned": {"$exists": true},
        },
    });

    // Find
    assert.commandWorked(
        mongosDB.runCommand({
            find: "view",
            comment: "find_readPref",
            maxTimeMS: 5 * 60 * 1000,
            $readPreference: {mode: "nearest", tags: [{tag: "secondary"}]},
            readConcern: {level: "local"},
        }),
    );

    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shardSecondary,
        filter: {
            "ns": coll.getFullName(),
            "command.aggregate": coll.getName(),
            "command.comment": "find_readPref",
            "command.pipeline.$mergeCursors": {"$exists": false},
            "nreturned": {"$exists": true},
        },
    });

    // Count
    assert.commandWorked(
        mongosDB.runCommand({
            count: "view",
            comment: "count_readPref",
            $readPreference: {mode: "nearest", tags: [{tag: "secondary"}]},
            readConcern: {level: "local"},
        }),
    );

    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shardSecondary,
        filter: {
            "ns": coll.getFullName(),
            "command.aggregate": coll.getName(),
            "command.comment": "count_readPref",
            "command.pipeline.$mergeCursors": {"$exists": false},
            "nreturned": {"$exists": true},
        },
    });

    // Distinct
    assert.commandWorked(
        mongosDB.runCommand({
            distinct: "view",
            key: "a",
            comment: "distinct_readPref",
            $readPreference: {mode: "nearest", tags: [{tag: "secondary"}]},
            readConcern: {level: "local"},
        }),
    );

    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shardSecondary,
        filter: {
            "ns": coll.getFullName(),
            "command.aggregate": coll.getName(),
            "command.comment": "distinct_readPref",
            "command.pipeline.$mergeCursors": {"$exists": false},
            "nreturned": {"$exists": true},
        },
    });

    assert.commandWorked(shardSecondary.setProfilingLevel(0));
}

confirmOptionsInProfiler(st.rs1.getPrimary().getDB(mongosDB.getName()));

confirmReadPreference(st.rs0.getSecondary().getDB(mongosDB.getName()));
confirmReadPreference(st.rs1.getSecondary().getDB(mongosDB.getName()));

st.stop();
