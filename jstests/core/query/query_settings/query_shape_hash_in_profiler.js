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
// ]
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

describe("Query shape hash in profiler", function () {
    const collName = "test";
    const qsutils = new QuerySettingsUtils(db, collName);

    // Default profiling status specified for the suite.
    let profilingStatus;

    before(function () {
        qsutils.removeAllQuerySettings();

        // Make sure all queries are logged as being slow.
        profilingStatus = db.getProfilingStatus();
        assert.commandWorked(db.runCommand({profile: 2}));

        db.test.insertMany([
            {x: 4, y: 1},
            {x: 4, y: 2},
        ]);
    });

    after(function () {
        // Restore the default slow query logging threshold.
        assert.commandWorked(db.runCommand({profile: profilingStatus.was, slowms: profilingStatus.slowms}));
    });

    // Finds the query shape hash from profiler output where the query has comment 'queryComment'.
    function getQueryShapeHashFromQueryProfiler(queryComment, profilerCount) {
        const profiles = db.system.profile.find({"command.comment": queryComment}).sort({ts: -1}).toArray();
        assert.eq(profiles.length, profilerCount, "Couldn't find any profiling data");

        const lastProfile = profiles[0];
        assert(lastProfile.queryShapeHash, "Couldn't find query shape hash in query profiler's data");
        return lastProfile.queryShapeHash;
    }

    function testQueryShapeHash(query) {
        // Run command to hit the profiler.
        const result = assert.commandWorked(db.runCommand(qsutils.withoutDollarDB(query)));

        // Get query shape hash from query profiler.
        let profilerCount = 1;
        const queryProfilerQueryShapeHash = getQueryShapeHashFromQueryProfiler(query.comment, profilerCount);

        // If cursor is still present, issue a getMore and check for query shape hash being
        // reported.
        if (result.cursor) {
            const commandCursor = new DBCommandCursor(db, result, 1 /* batchSize */);
            while (commandCursor.hasNext()) {
                commandCursor.next();
                profilerCount++;
                assert.eq(
                    queryProfilerQueryShapeHash,
                    getQueryShapeHashFromQueryProfiler(query.comment, profilerCount),
                    "queryShapeHash mismatch in getMore",
                );
            }
        }

        // Make sure query shape hash from the profiler matches the one from explain.
        assert.eq(
            queryProfilerQueryShapeHash,
            qsutils.getQueryShapeHashFromExplain(query),
            "Query shape hash from the logs doesn't match the one from query settings",
        );
    }

    it("should be reported for find and getMore commands", function () {
        const query = qsutils.makeFindQueryInstance({
            filter: {x: 4},
            batchSize: 0,
            comment: "Query shape hash in profiler test. Find query.",
        });
        testQueryShapeHash(query);
    });

    it("should be reported for distinct commands", function () {
        const query = qsutils.makeDistinctQueryInstance({
            key: "x",
            query: {x: 4},
            comment: "Query shape hash in profiler test. Distinct query.",
        });
        testQueryShapeHash(query);
    });

    it("should be reported for aggregate and getMore commands", function () {
        const query = qsutils.makeAggregateQueryInstance({
            pipeline: [{$match: {x: 4}}],
            comment: "Query shape hash in profiler test. Aggregate query.",
            cursor: {
                batchSize: 0,
            },
        });
        testQueryShapeHash(query);
    });
});
