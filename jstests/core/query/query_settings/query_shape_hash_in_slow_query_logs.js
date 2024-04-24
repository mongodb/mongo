// Tests that query shape hash is logged for slow queries.
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
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const collName = "test";
const qsutils = new QuerySettingsUtils(db, collName);
qsutils.removeAllQuerySettings();

// Make sure all queries are logged as being slow.
assert.commandWorked(db.runCommand({profile: 1, slowms: -1}));

// Insert some data for $where: 'sleep(...)' to wait upon.
db.test.insert({x: 4});

// Finds the query shape hash from slow query logs where the query has comment 'queryComment'.
function getQueryShapeHashFromSlowQueryLog(queryComment) {
    const slowQueryLogs = assert.commandWorked(db.adminCommand({getLog: 'global'}))
                              .log
                              .map((entry) => {
                                  return JSON.parse(entry);
                              })
                              .filter((entry) => {
                                  return entry.msg == "Slow query" && entry.attr &&
                                      entry.attr.command &&
                                      entry.attr.command.comment == queryComment;
                              });
    assert.gt(slowQueryLogs.length, 0, "Didn't get any logs");
    return slowQueryLogs.pop().attr.queryShapeHash;
}

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

    // Run command to hit slow query logs and the profiler.
    assert.commandWorked(db.runCommand(qsutils.withoutDollarDB(query)));

    // Get query shape hash from slow query log.
    const slowLogQueryShapeHash = getQueryShapeHashFromSlowQueryLog(query.comment);
    assert(slowLogQueryShapeHash, "Couldn't find query shape hash in slow queries log");

    // Get query shape hash from query profiler.
    const queryProfilerQueryShapeHash = getQueryShapeHashFromQueryProfiler(query.comment);
    assert(queryProfilerQueryShapeHash, "Couldn't find query shape hash in query profiler's data");

    // Expect the same shape hash in both cases.
    assert.eq(slowLogQueryShapeHash, queryProfilerQueryShapeHash, "Query shape hashes don't match");

    qsutils.withQuerySettings(query, querySettings, () => {
        // Make sure query settings are applied.
        const querySettingsQueryShapeHash = qsutils.getQueryHashFromQuerySettings(query);
        assert(querySettingsQueryShapeHash,
               `Couldn't find query settings for provided query: ${JSON.stringify(query)}`);

        // Make sure query shape hash from the logs matches the one from query settings.
        assert.eq(slowLogQueryShapeHash,
                  querySettingsQueryShapeHash,
                  "Query shape hash from the logs doesn't match the one from query settings");
    })
}

{
    // Test find.
    const query = qsutils.makeFindQueryInstance(
        {filter: {x: 4}, comment: "Query shape hash in slow query logs test. Find query."});
    testQueryShapeHash(query);
}

{
    // Test distinct.
    const query = qsutils.makeDistinctQueryInstance({
        key: "x",
        query: {x: 4},
        comment: "Query shape hash in slow query logs test. Distinct query."
    });
    testQueryShapeHash(query);
}

{
    // Test aggregate.
    const query = qsutils.makeAggregateQueryInstance({
        pipeline: [{$match: {x: 4}}],
        comment: "Query shape hash in slow query logs test. Aggregate query.",
        cursor: {}
    });
    testQueryShapeHash(query);
}
