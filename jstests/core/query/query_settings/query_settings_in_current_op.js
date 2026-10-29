/**
 * Confirms that $currentOp reports resolved query settings, including on getMore.
 *
 * @tags: [
 *   assumes_read_preference_unchanged,
 *   command_not_supported_in_serverless,
 *   directly_against_shardsvrs_incompatible,
 *   transitioning_replicaset_incompatible,
 *   requires_fcv_91,
 *   uses_parallel_shell,
 *   requires_getmore,
 *   assumes_no_implicit_cursor_exhaustion,
 *   # The find command is rewritten before reaching the failpoint on timeseries collections.
 *   exclude_from_timeseries_crud_passthrough,
 *   # Views bypass the find failpoint, and implicit view redirects change the cursor namespace.
 *   incompatible_with_views,
 * ]
 */

import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {waitForCurOpByComment, waitForCurOpByFailPointNoNS} from "jstests/libs/curop_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

// Failpoints and parallel shells must use the same mongos.
TestData.pinToSingleMongos = true;

describe("Query settings in $currentOp", function () {
    let coll;
    let qsutils;
    let session;
    let testDB;

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

        // A session is needed so that getMore can run with the same 'lsid' as the find/aggregate
        // that created the cursor.
        session = db.getMongo().startSession({causalConsistency: false});
        testDB = session.getDatabase(db.getName());
    });

    after(function () {
        session.endSession();
        assertDropCollection(db, coll.getName());
    });

    /**
     * Runs 'cmd' until it hangs on 'failPointName', and returns the 'querySettings' that
     * $currentOp reports for it.
     */
    function getQuerySettingsFromCurrentOp(failPointName, cmd, {onShards = false} = {}) {
        const failPoints = onShards
            ? FixtureHelpers.mapOnEachShardNode({
                  db,
                  primaryNodeOnly: true,
                  func: (shardDB) =>
                      configureFailPoint(shardDB, failPointName, {comment: cmd.comment}),
              })
            : [configureFailPoint(db, failPointName, {comment: cmd.comment})];

        // Run 'cmd' in a parallel shell, so that it can hang on the fail point while this shell
        // inspects $currentOp.
        const cmdShell = startParallelShell(
            funWithArgs(
                (dbName, cmd) => {
                    assert.commandWorked(db.getSiblingDB(dbName).runCommand(cmd));
                },
                db.getName(),
                cmd,
            ),
            db.getMongo().port,
        );

        try {
            // Block until 'cmd' is hanging on the fail point.
            if (onShards) {
                // Once a shard is paused, mongos has resolved the settings and is awaiting its reply.
                waitForCurOpByFailPointNoNS(db, failPointName, {"command.comment": cmd.comment});
            } else {
                // Background operations must not satisfy the wait before this command is paused.
                waitForCurOpByFailPointNoNS(
                    db,
                    failPointName,
                    {"command.comment": cmd.comment},
                    {localOps: true},
                );
            }

            // Find 'cmd' among the hanging ops by its unique comment.
            const currentOps = waitForCurOpByComment(db, cmd.comment, {}, {localOps: true});
            assert.eq(currentOps.length, 1, "expected one currentOp", {currentOps});
            return currentOps[0].querySettings;
        } finally {
            // Release 'cmd' and wait for the parallel shell to finish, even if an assertion
            // above failed.
            failPoints.forEach((failPoint) => failPoint.off());
            cmdShell();
        }
    }

    function withOpenCursor(query, fn) {
        const result = assert.commandWorked(testDB.runCommand(qsutils.withoutDollarDB(query)));
        const cursorId = result.cursor.id;
        assert.neq(cursorId, NumberLong(0), "expected the cursor to stay open", {result});
        try {
            fn(cursorId);
        } finally {
            assert.commandWorked(
                testDB.runCommand({killCursors: coll.getName(), cursors: [cursorId]}),
            );
        }
    }

    it("for find should match the settings reported in explain", function () {
        const query = qsutils.makeFindQueryInstance({filter: {x: 4}, comment: UUID()});
        const settings = {queryFramework: "classic"};
        qsutils.withQuerySettings(query, settings, () => {
            assert.docEq(
                settings,
                getQuerySettingsFromCurrentOp(
                    "waitInFindBeforeMakingBatch",
                    qsutils.withoutDollarDB(query),
                ),
            );
            qsutils.assertExplainQuerySettings(query, settings);
        });
    });

    it("should omit settings when find has no configured settings", function () {
        const query = qsutils.makeFindQueryInstance({filter: {x: 4}, comment: UUID()});
        assert.eq(
            undefined,
            getQuerySettingsFromCurrentOp(
                "waitInFindBeforeMakingBatch",
                qsutils.withoutDollarDB(query),
            ),
        );
    });

    it("for distinct should match the settings reported in explain", function () {
        const query = qsutils.makeDistinctQueryInstance({key: "y", query: {x: 4}, comment: UUID()});
        const settings = {queryFramework: "classic"};
        qsutils.withQuerySettings(query, settings, () => {
            assert.docEq(
                settings,
                getQuerySettingsFromCurrentOp(
                    "waitAfterCommandFinishesExecution",
                    qsutils.withoutDollarDB(query),
                    {onShards: true},
                ),
            );
            qsutils.assertExplainQuerySettings(query, settings);
        });
    });

    it("should retain find settings on getMore", function () {
        const query = qsutils.makeFindQueryInstance({
            filter: {x: 4},
            batchSize: 0,
            comment: UUID(),
        });
        const settings = {queryFramework: "classic"};
        qsutils.withQuerySettings(query, settings, () => {
            withOpenCursor(query, (cursorId) => {
                assert.docEq(
                    settings,
                    getQuerySettingsFromCurrentOp("waitAfterPinningCursorBeforeGetMoreBatch", {
                        getMore: cursorId,
                        collection: coll.getName(),
                        batchSize: 1,
                        comment: query.comment,
                        lsid: session.getSessionId(),
                    }),
                    "getMore for a find should report the configured settings",
                );
            });
        });
    });

    it("should retain aggregate settings on getMore", function () {
        const query = qsutils.makeAggregateQueryInstance({
            pipeline: [{$match: {x: 4}}],
            cursor: {batchSize: 0},
            comment: UUID(),
        });
        const settings = {queryFramework: "classic"};
        qsutils.withQuerySettings(query, settings, () => {
            withOpenCursor(query, (cursorId) => {
                assert.docEq(
                    settings,
                    getQuerySettingsFromCurrentOp("waitAfterPinningCursorBeforeGetMoreBatch", {
                        getMore: cursorId,
                        collection: coll.getName(),
                        batchSize: 1,
                        comment: query.comment,
                        lsid: session.getSessionId(),
                    }),
                    "getMore for an aggregate should report the configured settings",
                );
            });
        });
    });
});
