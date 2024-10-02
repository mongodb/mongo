// Tests that query shape hash is present in the query profiler.
//
// @tags: [
//   requires_profiling,
//   # Uses $where operation.
//   requires_scripting,
//   directly_against_shardsvrs_incompatible,
//   # Profile command doesn't support stepdowns.
//   does_not_support_stepdowns,
//   simulate_atlas_proxy_incompatible,
//   requires_fcv_80,
// ]
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const collName = "test";
const qsutils = new QuerySettingsUtils(db, collName);

assert.commandWorked(db.runCommand({profile: 2}));

// Insert some data for $where: 'sleep(...)' to wait upon.
db.test.insert({x: 4});

// Finds the query shape hash from profiler output where the query has comment 'queryComment'.
function getQueryShapeHashFromQueryProfiler(queryComment) {
    const profiles =
        db.system.profile.find({"command.comment": queryComment}).sort({ts: -1}).limit(1).toArray();
    assert.eq(profiles.length, 1, "Couldn't find any profiling data");
    const profile = profiles[0];
    assert.eq(typeof profile.queryShapeHash, "string", "No query shape hash found");
    return profile.queryShapeHash;
}

function testQueryShapeHash(query) {
    const querySettings = {
        indexHints: {
            ns: {
                db: db.getName(),
                coll: collName,
            },
            allowedIndexes: [{x: 1}, {$natural: 1}]
        }
    };

    // Run command to hit the profiler.
    assert.commandWorked(db.runCommand(qsutils.withoutDollarDB(query)));

    // Get query shape hash from query profiler.
    const queryProfilerQueryShapeHash = getQueryShapeHashFromQueryProfiler(query.comment);
    assert(queryProfilerQueryShapeHash, "Couldn't find query shape hash in query profiler's data");

    qsutils.withQuerySettings(query, querySettings, () => {
        // Make sure query settings are applied.
        const querySettingsQueryShapeHash = qsutils.getQueryShapeHashFromQuerySettings(query);
        assert(querySettingsQueryShapeHash,
               `Couldn't find query settings for provided query: ${JSON.stringify(query)}`);
    });
}

{
    // Test find.
    const query = qsutils.makeFindQueryInstance(
        {filter: {x: 4}, comment: "Query shape hash in profiler test. Find query."});
    testQueryShapeHash(query);
}

{
    // Test distinct.
    const query = qsutils.makeDistinctQueryInstance(
        {key: "x", query: {x: 4}, comment: "Query shape hash in profiler test. Distinct query."});
    testQueryShapeHash(query);
}

{
    // Test aggregate.
    const query = qsutils.makeAggregateQueryInstance({
        pipeline: [{$match: {x: 4}}],
        comment: "Query shape hash in profiler test. Aggregate query.",
        cursor: {}
    });
    testQueryShapeHash(query);
}
