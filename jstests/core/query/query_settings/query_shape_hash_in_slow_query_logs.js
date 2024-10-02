// Tests that query shape hash is logged for slow queries.
//
// @tags: [
//   assumes_read_preference_unchanged,
//   # Cowardly refusing to run test that interacts with the system profiler as the 'system.profile'
//   # collection is not replicated.
//   does_not_support_causal_consistency,
//   # The test expects one entry in the slow logs, therefore if the query is run multiple times the
//   # assertion will fail.
//   does_not_support_repeated_reads,
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
qsutils.removeAllQuerySettings();

// Make sure all queries are logged as being slow.
assert.commandWorked(db.runCommand({profile: 0, slowms: -1}));

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
                                      entry.attr.command && entry.attr.queryShapeHash &&
                                      entry.attr.command.comment == queryComment;
                              });

    // Assert that there is exactly one slow query log with given 'queryComment':
    // - for replica set, only the node that executes the query reports one log.
    // - for sharded cluster, only the mongos reports one log.
    assert.eq(slowQueryLogs.length,
              1,
              "Expected exactly one slow query log with 'queryShapeHash' and this query comment: " +
                  tojson(queryComment));
    return slowQueryLogs.pop().attr.queryShapeHash;
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

    // Run command to hit slow query logs.
    assert.commandWorked(db.runCommand(qsutils.withoutDollarDB(query)));

    // Get query shape hash from slow query log.
    const slowLogQueryShapeHash = getQueryShapeHashFromSlowQueryLog(query.comment);
    assert(slowLogQueryShapeHash, "Couldn't find query shape hash in slow queries log");

    qsutils.withQuerySettings(query, querySettings, () => {
        // Make sure query settings are applied.
        const querySettingsQueryShapeHash = qsutils.getQueryShapeHashFromQuerySettings(query);
        assert(querySettingsQueryShapeHash,
               `Couldn't find query settings for provided query: ${JSON.stringify(query)}`);

        // Make sure query shape hash from the logs matches the one from query settings.
        assert.eq(slowLogQueryShapeHash,
                  querySettingsQueryShapeHash,
                  "Query shape hash from the logs doesn't match the one from query settings");
    });
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
