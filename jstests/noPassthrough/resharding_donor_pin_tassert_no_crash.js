/**
 * Regression test for SERVER-134132
 *
 * ReshardingOpObserver::onUpdate and onDelete both unconditionally call _doPin(), which routes
 * through the same _calculatePin() and its tassert requiring MODE_X. onInsert returns early for
 * the donor namespace and never reaches _doPin() at all, so it is exercised here only to confirm
 * it is unaffected, not because it needed the tassert conversion.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("ReshardingOpObserver donor pin lock-mode tassert", function () {
    let rst;
    let primary;
    const kDonorColl = "localReshardingOperations.donor";
    const kTassertErrorCode = 13413201;

    function makeDonorDoc(ns) {
        return {
            _id: UUID(),
            ns: ns,
            ui: UUID(),
            tempNs: "test.system.resharding." + UUID().hex(),
            reshardingKey: {x: 1},
            mutableState: {state: "unused"},
        };
    }

    before(function () {
        rst = new ReplSetTest({nodes: 1, nodeOptions: {shardsvr: ""}});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
    });

    after(function () {
        // stopSet() stops nodes with waitpid:false, which can't verify a non-default exit code.
        rst.stop(0, undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
    });

    // Run first: onInsert returns early for this namespace, so this must succeed cleanly and
    // leave no tripwire behind, independent of the update/delete tests below.
    it("insert does not reach _doPin()/_calculatePin()", function () {
        assert.commandWorked(
            primary.getDB("config").runCommand({
                insert: kDonorColl,
                documents: [makeDonorDoc("test.insertOnly")],
            }),
        );
    });

    it("fails an update cleanly instead of crashing the server", function () {
        assert.commandWorked(
            primary
                .getDB("config")
                .runCommand({insert: kDonorColl, documents: [makeDonorDoc("test.forUpdate")]}),
        );

        // A plain update only takes MODE_IX; MODE_X is only held by the donor service itself.
        const res = primary.getDB("config").runCommand({
            update: kDonorColl,
            updates: [{q: {ns: "test.forUpdate"}, u: {$set: {ns: "test.updated"}}}],
        });

        const actualCode = res.ok ? res.writeErrors?.[0]?.code : res.code;
        assert.eq(actualCode, kTassertErrorCode, "expected the tassert's error code", {res});

        // Regression check: before the tassert conversion, this ping would fail because the
        // process had already aborted.
        assert.commandWorked(primary.adminCommand({ping: 1}));

        // The tassert fires as the first statement in _calculatePin(), before any cursor scan or
        // storage-engine pin/unpin call, and throwing there aborts the write's WriteUnitOfWork.
        // Confirm the update never actually applied, so a rejected attack leaves no partially
        // committed state (e.g. a document mutated with no corresponding pin recalculation)
        // behind for an attacker to exploit via repetition.
        assert.eq(
            1,
            primary.getDB("config")[kDonorColl].countDocuments({ns: "test.forUpdate"}),
            "the rejected update should not have modified the document",
        );
        assert.eq(0, primary.getDB("config")[kDonorColl].countDocuments({ns: "test.updated"}));
    });

    it("fails a delete cleanly instead of crashing the server", function () {
        assert.commandWorked(
            primary
                .getDB("config")
                .runCommand({insert: kDonorColl, documents: [makeDonorDoc("test.forDelete")]}),
        );

        // onDelete calls the same _doPin()/_calculatePin() as onUpdate; a plain delete only takes
        // MODE_IX.
        const res = primary.getDB("config").runCommand({
            delete: kDonorColl,
            deletes: [{q: {ns: "test.forDelete"}, limit: 1}],
        });

        const actualCode = res.ok ? res.writeErrors?.[0]?.code : res.code;
        assert.eq(actualCode, kTassertErrorCode, "expected the tassert's error code", {res});

        assert.commandWorked(primary.adminCommand({ping: 1}));

        // Same rollback guarantee as the update case above: the rejected delete must not have
        // removed the document.
        assert.eq(
            1,
            primary.getDB("config")[kDonorColl].countDocuments({ns: "test.forDelete"}),
            "the rejected delete should not have removed the document",
        );
    });
});
