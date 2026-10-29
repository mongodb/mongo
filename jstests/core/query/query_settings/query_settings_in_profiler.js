/**
 * Tests resolved query settings in profiler records and profiling filters, including on getMore.
 *
 * @tags: [
 *   # Profile records are local to the node that executes the query.
 *   assumes_read_preference_unchanged,
 *   # Query settings must be configured through mongos or a regular replica set.
 *   directly_against_shardsvrs_incompatible,
 *   # Causal-consistency overrides reject access to the non-replicated system.profile collection.
 *   does_not_support_causal_consistency,
 *   # Repeated-read overrides can add profiler records for auxiliary commands.
 *   does_not_support_repeated_reads,
 *   # Profiling configuration and open cursors must survive until their records are checked.
 *   does_not_support_stepdowns,
 *   # The view override redirects reads without rewriting setQuerySettings representatives.
 *   incompatible_with_views,
 *   # The profile command does not support signed security tokens.
 *   not_allowed_with_signed_security_token,
 *   # Query settings in profiler records were added in 9.1.
 *   requires_fcv_91,
 *   requires_getmore,
 *   requires_profiling,
 *   # TODO(SERVER-113800): Enable setClusterParameters with replicaset started with --shardsvr.
 *   transitioning_replicaset_incompatible,
 * ]
 */

import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {profilerHasZeroMatchingEntriesOrThrow} from "jstests/libs/profiler.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

describe("Query settings in profiler", function () {
    const settings = {queryFramework: "classic"};
    let coll;
    let qsutils;
    let profilingStatus;

    before(function () {
        coll = assertDropAndRecreateCollection(db, jsTestName());
        qsutils = new QuerySettingsUtils(db, coll.getName());
        qsutils.removeAllQuerySettings();
        assert.commandWorked(
            coll.insertMany([
                {x: 4, y: 1},
                {x: 4, y: 2},
            ]),
        );
        profilingStatus = db.getProfilingStatus();
        assert.commandWorked(db.setProfilingLevel(2, {filter: "unset"}));
    });

    after(function () {
        assert.commandWorked(
            db.setProfilingLevel(profilingStatus.was, {
                slowms: profilingStatus.slowms,
                sampleRate: profilingStatus.sampleRate,
                filter: profilingStatus.filter ?? "unset",
            }),
        );
        assertDropCollection(db, coll.getName());
    });

    function assertProfiles(expectedSettings, filter) {
        const profiles = db.system.profile.find(filter).toArray();
        assert.lt(0, profiles.length, "Expected matching profile records", {filter});

        for (const profile of profiles) {
            qsutils.assertEqualSettings(expectedSettings, profile.querySettings);
        }
    }

    function checkQuery(expectedSettings, query) {
        const result = assert.commandWorked(db.runCommand(qsutils.withoutDollarDB(query)));
        assertProfiles(expectedSettings, {"command.comment": query.comment});

        if (result.cursor) {
            const commandCursor = new DBCommandCursor(db, result, 1 /* batchSize */);
            try {
                commandCursor.itcount();
                assertProfiles(expectedSettings, {"command.comment": query.comment, op: "getmore"});
            } finally {
                commandCursor.close();
            }
        }
    }

    const makeQueries = {
        find: () => qsutils.makeFindQueryInstance({filter: {x: 4}, batchSize: 0, comment: UUID()}),
        distinct: () =>
            qsutils.makeDistinctQueryInstance({key: "x", query: {x: 4}, comment: UUID()}),
        aggregate: () =>
            qsutils.makeAggregateQueryInstance({
                pipeline: [{$match: {x: 4}}],
                cursor: {batchSize: 0},
                comment: UUID(),
            }),
    };

    for (const [commandName, makeQuery] of Object.entries(makeQueries)) {
        it(`reports ${commandName} settings`, function () {
            const query = makeQuery();
            qsutils.withQuerySettings(query, settings, () => checkQuery(settings, query));
        });

        it(`omits settings for ${commandName} without configured settings`, function () {
            const query = makeQuery();
            checkQuery(undefined, query);
        });
    }

    it("profiles queries with settings and excludes queries without settings", function () {
        const query = qsutils.makeFindQueryInstance({filter: {x: 4}, comment: UUID()});
        assert.commandWorked(
            db.setProfilingLevel(1, {filter: {"querySettings.queryFramework": "classic"}}),
        );

        try {
            qsutils.withQuerySettings(query, settings, () => {
                assert.commandWorked(db.runCommand(qsutils.withoutDollarDB(query)));
                assertProfiles(settings, {"command.comment": query.comment});
            });

            // Use a fresh comment to distinguish the query that must be excluded by the filter.
            query.comment = UUID();
            assert.commandWorked(db.runCommand(qsutils.withoutDollarDB(query)));
            profilerHasZeroMatchingEntriesOrThrow({
                profileDB: db,
                filter: {"command.comment": query.comment},
            });
        } finally {
            assert.commandWorked(db.setProfilingLevel(2, {filter: "unset"}));
        }
    });
});
