/**
 * Tests that the rollback procedure will update the 'config.transactions' table to be consistent
 * with the node data at the 'stableTimestamp', specifically in the case where multiple delete ops
 * to the 'config.transactions' table were coalesced into a single operation during secondary oplog
 * application.
 *
 * @tags: [requires_persistence]
 */

import {
    runTests
} from
    "jstests/replsets/libs/rollback_with_coalesced_txn_table_updates_during_oplog_application_helper.js";

const initFunc = (primary, ns, counterTotal) => {
    assert.commandWorked(primary.getCollection(ns).runCommand("insert", {
        documents: Array.from({length: counterTotal}, (_, i) => ({_id: i})),
        writeConcern: {w: 5}
    }));
};

const stopReplProducerOnDocumentFunc = (counterMajorityCommitted) => {
    return {document: {"_id": counterMajorityCommitted + 1}};
};

const opsFunc = (primary, ns, counterTotal, lsid) => {
    assert.commandWorked(primary.getCollection(ns).runCommand("delete", {
        deletes: Array.from({length: counterTotal}, (_, i) => ({q: {_id: i}, limit: 1})),
        lsid,
        txnNumber: NumberLong(2),
    }));
};

const stmtMajorityCommittedFunc = (primary, ns, counterMajorityCommitted) => {
    return {ns, "o._id": counterMajorityCommitted, "op": "d"};
};

const validateFunc = (secondary1, ns, counterMajorityCommitted, counterTotal, lsid) => {
    // Insert doc in the range [0, counterMajorityCommitted] which should have been deleted.
    assert.commandWorked(secondary1.getCollection(ns).runCommand("insert", {
        documents: Array.from({length: counterMajorityCommitted + 1}, (_, i) => ({_id: i})),
        writeConcern: {w: 5}
    }));

    // Docs in the range [counterMajorityCommitted + 1, counterTotal - 1] should exist because the
    // delete statements were rolled back.
    for (var i = counterMajorityCommitted + 1; i < counterTotal; i++) {
        const docBeforeRetry = secondary1.getCollection(ns).findOne({_id: i});
        assert.eq(docBeforeRetry, {_id: i});
    }

    // Retry the operation which should only delete the range
    // [counterMajorityCommitted + 1, counterTotal - 1].
    assert.commandWorked(secondary1.getCollection(ns).runCommand("delete", {
        deletes: Array.from({length: counterTotal}, (_, i) => ({q: {_id: i}, limit: 1})),
        lsid,
        txnNumber: NumberLong(2),
        writeConcern: {w: 5},
    }));

    // We should still find the documents in the range [0, counterMajorityCommitted].
    for (var i = 0; i <= counterMajorityCommitted; i++) {
        const docAfterRetry = secondary1.getCollection(ns).findOne({_id: i});
        assert.eq(docAfterRetry, {_id: i});
    }

    // Docs in the range [counterMajorityCommitted + 1, counterTotal - 1] should be deleted by the
    // retry.
    for (var i = counterMajorityCommitted + 1; i < counterTotal; i++) {
        const docAfterRetry = secondary1.getCollection(ns).findOne({_id: i});
        assert.eq(docAfterRetry, null);
    }
};

runTests(
    initFunc, stopReplProducerOnDocumentFunc, opsFunc, stmtMajorityCommittedFunc, validateFunc);
