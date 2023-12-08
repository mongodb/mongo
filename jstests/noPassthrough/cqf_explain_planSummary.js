/**
 * Tests the planSummary field present in CQF explain output.
 */

import {getPlanSummaries} from "jstests/libs/analyze_plan.js"

function checkSummaries(db, collName, summaryExpected, hint, isSharded) {
    const commands = [
        // Empty queries
        {explain: {find: collName, filter: {}, hint: hint}, verbosity: "queryPlanner"},
        {
            explain: {aggregate: collName, pipeline: [], cursor: {}, hint: hint},
            verbosity: "queryPlanner"
        },

        // Single predicate
        {explain: {find: collName, filter: {a: {$gt: 10}}, hint: hint}, verbosity: "queryPlanner"},
        {
            explain:
                {aggregate: collName, pipeline: [{$match: {a: {$gt: 10}}}], cursor: {}, hint: hint},
            verbosity: "queryPlanner"
        },

        // Predicate + projection
        {
            explain: {find: collName, filter: {a: {$gt: 10}}, projection: {a: 1}, hint: hint},
            verbosity: "queryPlanner"
        },
        {
            explain: {
                aggregate: collName,
                pipeline: [{$match: {a: {$gt: 10}}}, {$project: {b: 1}}],
                cursor: {},
                hint: hint
            },
            verbosity: "queryPlanner"
        },

        // Multi predicate
        {
            explain: {find: collName, filter: {a: {$gt: 10}, b: {$lt: 5}}, hint: hint},
            verbosity: "queryPlanner"
        },
        {
            explain: {
                aggregate: collName,
                pipeline: [{$match: {a: {$gt: 10}, b: {$lt: 5}}}, {$project: {b: 1}}],
                cursor: {},
                hint: hint
            },
            verbosity: "queryPlanner"
        },
    ];

    for (const command of commands) {
        const explain = assert.commandWorked(db.runCommand(command));
        const summaries = getPlanSummaries(explain);

        if (summaryExpected == "EOF" || !isSharded) {
            assert.eq(summaries.length, 1, tojson(explain));
        } else {
            // There should be one summary per shard processing the query.
            assert.eq(summaries.length, 2, tojson(explain));
        }

        summaries.forEach(summary => assert.eq(
                              summary, summaryExpected, {explain: explain, summaries: summaries}));
    }
}

// Asserts that explain output for find and agg queries against sharded and unsharded collections
// with and without hints contain the expected plan summaries.
function runTest(conn, canShardColl) {
    const db = conn.getDB('test');
    const coll = db[jsTestName()];
    coll.drop();
    assert.commandWorked(coll.insertMany([...Array(100).keys()].map(i => {
        return {_id: i, a: i, b: i};
    })));

    // Test EOF planSummary
    checkSummaries(db,
                   'nonExistent' /* collName */,
                   "EOF" /* planSummary */,
                   {} /* hint */,
                   false /* isSharded */);

    // Test COLLSCAN planSummary...
    // 1) For a collection that has no indexes/is not sharded.
    checkSummaries(
        db, coll.getName(), "COLLSCAN" /* planSummary */, {} /* hint */, false /* isSharded */);

    // 2) For a collection that has indexes.
    assert.commandWorked(coll.createIndex({a: 1}));
    checkSummaries(db,
                   coll.getName(),
                   "COLLSCAN" /* planSummary */,
                   {$natural: 1} /* hint */,
                   false /* isSharded */);

    // 3) For a collection that is sharded.
    if (canShardColl) {
        conn.shardColl(coll.getName(), {a: 1}, {a: 50}, {a: 51});
        checkSummaries(db,
                       coll.getName(),
                       "COLLSCAN" /* planSummary */,
                       {$natural: 1} /* hint */,
                       true /* isSharded */);
    }
}

// Standalone
let conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagCommonQueryFramework: true,
        "failpoint.enableExplainInBonsai": tojson({mode: "alwaysOn"}),
        internalQueryFrameworkControl: 'tryBonsai',
    }
});
assert.neq(null, conn, "mongod was unable to start up");
runTest(conn, false /* canShardColl */);
MongoRunner.stopMongod(conn);

// Sharded
let shardingConn = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        shardOptions: {
            setParameter: {
                "failpoint.enableExplainInBonsai": tojson({mode: "alwaysOn"}),
                featureFlagCommonQueryFramework: true,
                internalQueryFrameworkControl: 'tryBonsai',
            }
        },
    }
});
runTest(shardingConn, true /* canShardColl */);
shardingConn.stop();
