/**
 * Tests that query settings and query knobs are reported in the slow query logs, together with the
 * source each knob value came from.
 *
 * @tags: [
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 *   # Incompatible with setParameter.
 *   does_not_support_stepdowns,
 *   # Query settings commands can not be run on the shards directly.
 *   directly_against_shardsvrs_incompatible,
 *   # TODO(SERVER-113800): Enable setClusterParameters with replicaset started with --shardsvr.
 *   transitioning_replicaset_incompatible,
 *   # Uses runWithParamsAllNonConfigNodes which requires a stable shard list.
 *   assumes_stable_shard_list,
 *   # The test expects an exact number of slow log entries per unique comment.
 *   does_not_support_repeated_reads,
 *   # Uses a $where clause to make the query slow enough to get logged. $where is not allowed on
 *   # timeseries collections.
 *   requires_scripting,
 *   exclude_from_timeseries_crud_passthrough,
 *   does_not_support_causal_consistency,
 *   does_not_support_transactions,
 *   featureFlagPqsQueryKnobs,
 *   requires_fcv_90,
 * ]
 */
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

// 'getLog' targets a specific mongos, so pin to a single mongos.
TestData.pinToSingleMongos = true;

describe("Query settings and query knobs in slow query logs", function () {
    // A fixed set of pqs-settable knobs covering the int, double and bool types, with non-default
    // values so each knob must be reported with a non-default source. setParameter addresses knobs
    // by server parameter name, while query settings and the slow logs use their wire name.
    const knobToVal = {
        internalQueryPlanEvaluationWorks: 12345,
        internalQueryPlanEvaluationCollFraction: 0.4,
        internalQueryPlanTieBreakingWithIndexHeuristics: false,
    };
    const queryKnobs = {
        planEvaluationWorks: 12345,
        planEvaluationCollFraction: 0.4,
        planTieBreakingWithIndexHeuristics: false,
    };

    let coll;
    let qsutils;
    let representativeQuery;

    before(function () {
        coll = assertDropAndRecreateCollection(db, jsTestName());
        assert.commandWorked(coll.insert({x: 1}));
        qsutils = new QuerySettingsUtils(db, coll.getName());
        qsutils.removeAllQuerySettings();
        // The $where sleep makes the query exceed the default 100ms 'slowms' threshold, so it
        // always gets logged as being slow.
        representativeQuery = qsutils.makeFindQueryInstance({
            filter: {$where: "sleep(200); return this.x === 1;"},
        });
    });

    after(function () {
        assertDropCollection(db, coll.getName());
    });

    /**
     * Utility to suffix comments with a unique identifier so each test case is matched against
     * exactly the slow log entries it produced.
     */
    function makeUniqueComment(label) {
        return `${label}-${UUID().toString()}`;
    }

    /**
     * Finds the single slow query log entry produced by the query with the given unique comment.
     */
    function getSlowQueryLogEntry(queryComment) {
        const slowQueryLogs = assert
            .commandWorked(db.adminCommand({getLog: "global"}))
            .log.map((entry) => JSON.parse(entry))
            .filter(
                (entry) =>
                    entry.msg == "Slow query" &&
                    entry.attr &&
                    entry.attr.command &&
                    entry.attr.command.comment == queryComment,
            );
        assert.gte(slowQueryLogs.length, 1, "expected at least one slow query log entry", {
            queryComment,
            slowQueryLogs,
        });
        return slowQueryLogs.pop();
    }

    /**
     * Runs the representative query with a unique comment and returns its slow log entry.
     */
    function runQueryAndGetSlowLogEntry(label) {
        const comment = makeUniqueComment(label);
        const queryCmd = {...qsutils.withoutDollarDB(representativeQuery), comment};
        assert.commandWorked(db.runCommand(queryCmd));
        return getSlowQueryLogEntry(comment);
    }

    /**
     * Asserts that the 'queryKnobs' entry of the given slow log entry reports every knob with the
     * given source.
     */
    function assertLoggedKnobs(entry, expectedSource) {
        const loggedKnobs = entry.attr.queryKnobs;
        assert(loggedKnobs, "expected a queryKnobs entry in the slow query log", {entry});
        for (const [wireName, expectedValue] of Object.entries(queryKnobs)) {
            const knob = loggedKnobs[wireName];
            assert(knob, "expected knob to appear in the slow query log", {wireName, entry});
            assert.eq(knob.source, expectedSource, "unexpected knob source", {wireName, knob});
            assert.eq(knob.value, expectedValue, "unexpected knob value", {wireName, knob});
        }
    }

    it("should report knobs set via setParameter with source 'setParameter'", function () {
        runWithParamsAllNonConfigNodes(db, knobToVal, () => {
            const entry = runQueryAndGetSlowLogEntry("Knobs via setParameter");
            assertLoggedKnobs(entry, "setParameter");
            // No query settings are set, so only the knobs must be reported.
            assert.eq(
                entry.attr.querySettings,
                undefined,
                "expected no querySettings entry in the slow query log",
                {entry},
            );
        });
    });

    it("should report knobs set via query settings in both the querySettings and queryKnobs entries", function () {
        qsutils.withQuerySettings(representativeQuery, {queryKnobs}, () => {
            const entry = runQueryAndGetSlowLogEntry("Knobs via query settings");
            assertLoggedKnobs(entry, "querySettings");

            // The applied query settings object must also be logged, including the knobs.
            const loggedSettings = entry.attr.querySettings;
            assert(loggedSettings, "expected a querySettings entry in the slow query log", {
                entry,
            });
            assert.docEq(
                queryKnobs,
                loggedSettings.queryKnobs,
                "expected the logged query settings to contain the knobs",
            );
        });
    });
});
