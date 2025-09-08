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
//   # Measures exact occurence of slow query logs.
//   requires_profiling,
//   directly_against_shardsvrs_incompatible,
//   # Profile command doesn't support stepdowns.
//   does_not_support_stepdowns,
//   simulate_atlas_proxy_incompatible,
//   # Does not support transactions as the test is issuing getMores and transaction can not be started with getMore.
//   does_not_support_transactions,
// ]
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

describe("QueryShapeHash in slow logs", function () {
    const collName = "test";
    const qsutils = new QuerySettingsUtils(db, collName);

    // Default profiling status specified for the suite.
    let profilingStatus;

    before(function () {
        qsutils.removeAllQuerySettings();

        // Make sure all queries are logged as being slow.
        profilingStatus = db.getProfilingStatus();
        assert.commandWorked(db.runCommand({profile: 0, slowms: -1}));

        db.test.insertMany([
            {x: 4, y: 1},
            {x: 4, y: 2},
        ]);
    });

    after(function () {
        // Restore the default slow query logging threshold.
        assert.commandWorked(db.runCommand({profile: profilingStatus.was, slowms: profilingStatus.slowms}));
    });

    // Finds the query shape hash from slow query logs where the query has comment 'queryComment'.
    function getQueryShapeHashFromSlowQueryLog(queryComment, expectedCount) {
        const slowQueryLogs = assert
            .commandWorked(db.adminCommand({getLog: "global"}))
            .log.map((entry) => {
                return JSON.parse(entry);
            })
            .filter((entry) => {
                return (
                    entry.msg == "Slow query" &&
                    entry.attr &&
                    entry.attr.command &&
                    entry.attr.queryShapeHash &&
                    entry.attr.command.comment == queryComment
                );
            });
        jsTest.log.debug(`Slow query logs`, {slowQueryLogs});

        // Assert that there is 'expectedCount' of slow query logs with given 'queryComment':
        // - for replica set, only the node that executes the query reports one log.
        // - for sharded cluster, only the mongos reports one log.
        assert.eq(
            slowQueryLogs.length,
            expectedCount,
            `Expected ${expectedCount} of slow query log with 'queryShapeHash' and this query comment: ${tojson(queryComment)}`,
        );
        return slowQueryLogs.pop().attr.queryShapeHash;
    }

    // Asserts that the query shape hash is found in slow query logs for the given comment.
    function assertQueryShapeHashFromSlowLogs(comment, expectedCount) {
        const slowLogQueryShapeHash = getQueryShapeHashFromSlowQueryLog(comment, expectedCount);
        assert(slowLogQueryShapeHash, "Couldn't find query shape hash in slow queries log");
        return slowLogQueryShapeHash;
    }

    function testQueryShapeHash(query) {
        // Run command to hit slow query logs.
        const result = assert.commandWorked(db.runCommand(qsutils.withoutDollarDB(query)));

        // Get query shape hash from slow query log.
        let slowLogsCount = 1;
        const slowLogQueryShapeHash = assertQueryShapeHashFromSlowLogs(query.comment, slowLogsCount);

        // If cursor is still present, issue a getMore and check for query shape hash being
        // reported.
        if (result.cursor) {
            const commandCursor = new DBCommandCursor(db, result, 1 /* batchSize */);
            while (commandCursor.hasNext()) {
                commandCursor.next();
                slowLogsCount++;
                assert.eq(
                    slowLogQueryShapeHash,
                    assertQueryShapeHashFromSlowLogs(query.comment, slowLogsCount),
                    "queryShapeHash mismatch in getMore",
                );
            }
        }

        // Make sure query shape hash from the logs matches the one from explain.
        assert.eq(
            slowLogQueryShapeHash,
            qsutils.getQueryShapeHashFromExplain(query),
            "Query shape hash from the logs doesn't match the one from query settings",
        );
    }

    it("should be reported for find and getMore commands", function () {
        const query = qsutils.makeFindQueryInstance({
            filter: {x: 4},
            batchSize: 0,
            comment: "Query shape hash in slow query logs test. Find query.",
        });
        testQueryShapeHash(query);
    });

    it("should be reported for distinct commands", function () {
        const query = qsutils.makeDistinctQueryInstance({
            key: "x",
            query: {x: 4},
            comment: "Query shape hash in slow query logs test. Distinct query.",
        });
        testQueryShapeHash(query);
    });

    it("should be reported for aggregate and getMore commands", function () {
        const query = qsutils.makeAggregateQueryInstance({
            pipeline: [{$match: {x: 4}}],
            comment: "Query shape hash in slow query logs test. Aggregate query.",
            cursor: {
                batchSize: 0,
            },
        });
        testQueryShapeHash(query);
    });
});
