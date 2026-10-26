/**
 * Steps down a primary while an index build waits for a prepared transaction. Verifies that the
 * index build finishes after the node is re-elected and the transaction commits.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {waitForState} from "jstests/replsets/rslib.js";

describe("completes an index build after stepdown while a transaction is prepared", function () {
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

    it("completes an index build after stepdown while a transaction is prepared", function () {
        assert.commandWorked(coll.insert({_id: 0, a: 0}));
        const bulkLoad = configureFailPoint(primary, "hangAfterIndexBuildDumpsInsertsFromBulk");
        const awaitBuild = IndexBuildTest.startIndexBuild(
            primary,
            coll.getFullName(),
            {a: 1},
            {},
            ErrorCodes.InterruptedDueToReplStateChange,
        );
        bulkLoad.wait();

        const session = primary.startSession();
        const sessionColl = session
            .getDatabase(coll.getDB().getName())
            .getCollection(coll.getName());
        session.startTransaction();
        assert.commandWorked(sessionColl.insert({_id: 1, a: 1}));
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

        // The drain must see prepare even when it reads from a majority snapshot.
        rst.awaitLastOpCommitted();
        bulkLoad.off();

        const opId = IndexBuildTest.waitForIndexBuildToStart(coll.getDB(), coll.getName(), "a_1", {
            desc: {$regex: /IndexBuildsCoordinatorMongod/},
        });
        assert.soon(() => {
            const [op] = coll.getDB().currentOp({opid: opId, $all: true}).inprog;
            return op && op.prepareReadConflicts > 0 && Object.keys(op.locks || {}).length === 0;
        });

        assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));
        waitForState(primary, ReplSetTest.State.SECONDARY);
        awaitBuild();

        assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
        rst.getPrimary();

        // Stepdown completed while T was still prepared. Resolve and let the build finish.
        assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
        IndexBuildTest.waitForIndexBuildToStop(coll.getDB(), coll.getName(), "a_1");
        IndexBuildTest.assertIndexesSoon(coll, 2, ["_id_", "a_1"], [], {
            includeBuildUUIDs: true,
        });

        session.endSession();
    });
});
