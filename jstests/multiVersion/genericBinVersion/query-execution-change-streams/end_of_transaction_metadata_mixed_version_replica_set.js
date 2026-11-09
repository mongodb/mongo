/**
 * Verifies that single replica-set transactions replicate between a latest primary and a last-LTS
 * secondary in a mixed-version replica set, in both directions, and that the end-of-transaction
 * marker the latest binary writes is tolerated by the last-LTS binary.
 *
 * A replica-set transaction has two commit shapes:
 *
 *  - unprepared: it commits through a final 'applyOps' entry, which the latest binary marks with
 *    'allowEndOfTransaction: true' on its 'o' when the end-of-transaction write path is enabled;
 *  - prepared: it commits through a standalone 'commitTransaction' entry, which the latest binary
 *    marks with 'allowEndOfTransaction: true' on its 'o2' (the 'commitTransaction' entry's 'o' is
 *    parsed strictly, so it cannot carry a field the last-LTS binary does not know).
 *
 * The end-of-transaction parameters are enabled only on the latest node, because the last-LTS
 * binary knows neither the new feature flag nor the new server parameter.
 *
 * In every case the transaction must commit, its documents must be visible on both the primary and
 * the secondary, and the marker (or its absence, when the last-LTS binary is the primary) must be as
 * expected.
 *
 * @tags: [
 *   uses_transactions,
 *   uses_prepare_transaction,
 *   requires_persistence,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {withSession} from "jstests/libs/query/change_stream_util.js";

const dbName = jsTestName();
const collName = "coll";
const ns = dbName + "." + collName;

// Turns on the end-of-transaction write path (feature flag plus kill switch). Only the latest binary
// understands these; the last-LTS binary knows neither the feature flag nor the parameter.
const enableEndOfTransactionParams = {
    featureFlagEndOfTransactionChangeEventSynthesis: true,
    allowEndOfTransaction: true,
};

// Builds the per-node options for a node running 'binVersion', enabling the end-of-transaction
// parameters on the versions that understand them (the latest binary only).
function nodeOptionsForBinVersion(binVersion, rsConfig) {
    const options = {binVersion, rsConfig};
    if (binVersion === "latest") {
        options.setParameter = enableEndOfTransactionParams;
    }
    return options;
}

// Starts a 2-node replica set whose node 0 runs 'primaryBinVersion' and node 1 runs
// 'secondaryBinVersion'. Node 0 is given the higher priority so that it stays the primary.
function startMixedVersionReplSet(primaryBinVersion, secondaryBinVersion) {
    const rst = new ReplSetTest({
        nodes: [
            nodeOptionsForBinVersion(primaryBinVersion, {priority: 2}),
            nodeOptionsForBinVersion(secondaryBinVersion, {priority: 1}),
        ],
    });
    rst.startSet();
    rst.initiate();
    assert.eq(rst.nodes[0].host, rst.getPrimary().host, "expected node 0 to be the primary");
    return rst;
}

// Commits an unprepared multi-document transaction inserting 'docs' on the primary of 'rst'.
// Returns the session id and transaction number that identify the transaction's oplog entries.
function commitUnpreparedTransaction(rst, docs) {
    let lsid, txnNumber;
    withSession(rst.getPrimary(), (session) => {
        const sessionColl = session.getDatabase(dbName)[collName];
        session.startTransaction();
        for (const doc of docs) {
            assert.commandWorked(sessionColl.insert(doc));
        }
        assert.commandWorked(session.commitTransaction_forTesting());
        lsid = session.getSessionId();
        txnNumber = session.getTxnNumber_forTesting();
    });
    return {lsid, txnNumber};
}

// Commits a prepared transaction inserting 'docs' on the primary of 'rst'. Returns the session id
// and transaction number that identify the transaction's oplog entries.
function commitPreparedTransaction(rst, docs) {
    let lsid, txnNumber;
    withSession(rst.getPrimary(), (session) => {
        const sessionColl = session.getDatabase(dbName)[collName];
        session.startTransaction();
        for (const doc of docs) {
            assert.commandWorked(sessionColl.insert(doc));
        }
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
        assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
        lsid = session.getSessionId();
        txnNumber = session.getTxnNumber_forTesting();
    });
    return {lsid, txnNumber};
}

// Asserts that every document in 'docs' is visible on both the primary and the secondary of 'rst'.
function assertVisibleOnBothNodes(rst, docs) {
    for (const node of [rst.getPrimary(), rst.getSecondary()]) {
        for (const doc of docs) {
            assert.soon(
                () => node.getCollection(ns).findOne(doc) !== null,
                "expected the committed document to be visible",
                undefined,
                undefined,
                undefined,
                {node: node.host, doc},
            );
        }
    }
}

// Returns the final 'applyOps' oplog entry the primary of 'rst' wrote for the transaction with the
// given session and transaction number.
function getFinalApplyOpsEntry(rst, lsid, txnNumber) {
    const entries = rst
        .getPrimary()
        .getDB("local")
        .oplog.rs.find({"lsid.id": lsid.id, txnNumber, "o.applyOps": {$exists: true}})
        .sort({ts: 1})
        .toArray();
    assert.gt(entries.length, 0, "expected at least one applyOps entry", {lsid, txnNumber});
    return entries[entries.length - 1];
}

// Returns the single 'commitTransaction' oplog entry the primary of 'rst' wrote for the transaction
// with the given session and transaction number.
function getCommitTransactionEntry(rst, lsid, txnNumber) {
    let entry;
    assert.soon(
        () => {
            const entries = rst
                .getPrimary()
                .getDB("local")
                .oplog.rs.find({"lsid.id": lsid.id, txnNumber, "o.commitTransaction": 1})
                .sort({ts: 1})
                .toArray();
            if (entries.length !== 1) {
                return false;
            }
            entry = entries[0];
            return true;
        },
        "expected exactly one commitTransaction entry",
        undefined,
        undefined,
        undefined,
        {lsid, txnNumber},
    );
    return entry;
}

describe("commit on a latest primary, replicated to a last-LTS secondary", function () {
    let rst;

    before(function () {
        rst = startMixedVersionReplSet("latest", "last-lts");
    });

    after(function () {
        rst.stopSet();
    });

    it("marks an unprepared transaction's final applyOps entry and replicates it", function () {
        const docs = [{_id: 1}, {_id: 2}];
        const {lsid, txnNumber} = commitUnpreparedTransaction(rst, docs);

        const entry = getFinalApplyOpsEntry(rst, lsid, txnNumber);
        assert.eq(
            true,
            entry.o.allowEndOfTransaction,
            "the latest primary should mark the final applyOps entry",
            {entry},
        );

        // The last-LTS secondary tolerates the marker and applies the transaction.
        assertVisibleOnBothNodes(rst, docs);
    });

    it("marks a prepared transaction's commitTransaction entry on 'o2' and replicates it", function () {
        const docs = [{_id: 3}, {_id: 4}];
        const {lsid, txnNumber} = commitPreparedTransaction(rst, docs);

        const entry = getCommitTransactionEntry(rst, lsid, txnNumber);
        assert.eq(
            true,
            entry.o2 && entry.o2.allowEndOfTransaction,
            "the latest primary should mark the commitTransaction entry's 'o2'",
            {entry},
        );
        assert(
            !entry.o.allowEndOfTransaction,
            "the marker must not be written on the strictly parsed 'o'",
            {entry},
        );

        // The last-LTS secondary tolerates the marker and applies the transaction.
        assertVisibleOnBothNodes(rst, docs);
    });
});

describe("commit on a last-LTS primary, replicated to a latest secondary", function () {
    let rst;

    before(function () {
        rst = startMixedVersionReplSet("last-lts", "latest");
    });

    after(function () {
        rst.stopSet();
    });

    it("replicates an unprepared transaction without the marker to the latest secondary", function () {
        const docs = [{_id: 1}, {_id: 2}];
        const {lsid, txnNumber} = commitUnpreparedTransaction(rst, docs);

        const entry = getFinalApplyOpsEntry(rst, lsid, txnNumber);
        assert(
            !entry.o.allowEndOfTransaction,
            "the last-LTS primary does not know the marker and must not write it",
            {entry},
        );

        assertVisibleOnBothNodes(rst, docs);
    });

    it("replicates a prepared transaction without the marker to the latest secondary", function () {
        const docs = [{_id: 3}, {_id: 4}];
        const {lsid, txnNumber} = commitPreparedTransaction(rst, docs);

        const entry = getCommitTransactionEntry(rst, lsid, txnNumber);
        assert(
            !(entry.o2 && entry.o2.allowEndOfTransaction),
            "the last-LTS primary must not write the marker",
            {entry},
        );

        assertVisibleOnBothNodes(rst, docs);
    });
});
