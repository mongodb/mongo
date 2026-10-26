/**
 * Shuts down a server while an index build and a prepared transaction are in progress. Verifies
 * that shutdown completes and, after restart, the transaction commits and the index build finishes.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

describe("recovers an index build after shutdown while a transaction is prepared", function () {
    let rst, primary, coll;

    before(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        coll = primary.getDB(jsTestName()).coll;
    });

    after(function () {
        rst.stopSet();
    });

    it("recovers an index build after shutdown while a transaction is prepared", function () {
        assert.commandWorked(coll.insert({_id: 0, a: -1}));
        const bulk = configureFailPoint(primary, "hangAfterIndexBuildDumpsInsertsFromBulk");
        const awaitBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {}, [
            ErrorCodes.InterruptedAtShutdown,
            ErrorCodes.ShutdownInProgress,
            ErrorCodes.InterruptedDueToReplStateChange,
        ]);
        bulk.wait();

        const session = primary.startSession();
        const sessionColl = session
            .getDatabase(coll.getDB().getName())
            .getCollection(coll.getName());
        session.startTransaction();
        assert.commandWorked(sessionColl.insert({_id: 1, a: 640}));
        assert.commandWorked(sessionColl.update({_id: 1}, {$set: {a: 1}}));
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

        // The drain must see prepare even when it reads from a majority snapshot.
        rst.awaitLastOpCommitted();
        bulk.off();

        const opId = IndexBuildTest.waitForIndexBuildToStart(coll.getDB(), coll.getName(), "a_1", {
            desc: {$regex: /IndexBuildsCoordinatorMongod/},
        });
        assert.soon(() => {
            const [op] = coll.getDB().currentOp({opid: opId, $all: true}).inprog;
            return op && op.prepareReadConflicts > 0 && Object.keys(op.locks || {}).length === 0;
        });

        rst.stop(primary, undefined, {skipValidation: true});

        // Shutdown may close the shell's connection before returning a command error.
        awaitBuild({checkExitSuccess: false});

        const restarted = rst.start(primary, {}, true);
        rst.getPrimary();

        const recoveredSession = new _DelegatingDriverSession(restarted, session);
        PrepareHelpers.awaitMajorityCommitted(rst, prepareTimestamp);
        assert.commandWorked(PrepareHelpers.commitTransaction(recoveredSession, prepareTimestamp));

        const recoveredColl = restarted.getDB(coll.getDB().getName()).getCollection(coll.getName());
        IndexBuildTest.assertIndexesSoon(recoveredColl, 2, ["_id_", "a_1"], [], {
            includeBuildUUIDs: true,
        });

        assert.commandWorked(restarted.adminCommand({endSessions: [session.getSessionId()]}));
    });
});
