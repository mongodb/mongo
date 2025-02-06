/**
 * Tests that the rollback procedure will update the 'config.transactions' table to be consistent
 * with the node data at the 'stableTimestamp', specifically in the case where multiple update ops
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
    assert.commandWorked(
        primary.getCollection(ns).insert({_id: 0, counter: 0}, {writeConcern: {w: 5}}));
};

const stopReplProducerOnDocumentFunc = (counterMajorityCommitted) => {
    return {document: {"diff.u.counter": counterMajorityCommitted + 1}};
};

const opsFunc = (primary, ns, counterTotal, lsid) => {
    assert.commandWorked(primary.getCollection(ns).runCommand("update", {
        updates: Array.from({length: counterTotal}, () => ({q: {_id: 0}, u: {$inc: {counter: 1}}})),
        lsid,
        txnNumber: NumberLong(2),
    }));
};

const stmtMajorityCommittedFunc = (primary, ns, counterMajorityCommitted) => {
    return {ns, "o.diff.u.counter": counterMajorityCommitted};
};

const validateFunc = (secondary1, ns, counterMajorityCommitted, counterTotal, lsid) => {
    const docBeforeRetry = secondary1.getCollection(ns).findOne({_id: 0});
    assert.eq(docBeforeRetry, {_id: 0, counter: counterMajorityCommitted});

    assert.commandWorked(secondary1.getCollection(ns).runCommand("update", {
        updates: Array.from({length: counterTotal}, () => ({q: {_id: 0}, u: {$inc: {counter: 1}}})),
        lsid,
        txnNumber: NumberLong(2),
        writeConcern: {w: 5},
    }));

    // Make sure we don't re-execute operations that have already been updated by making sure that
    // counter equals exactly the counterTotal after the retry.
    const docAfterRetry = secondary1.getCollection(ns).findOne({_id: 0});
    assert.eq(docAfterRetry, {_id: 0, counter: counterTotal});
};

runTests(
    initFunc, stopReplProducerOnDocumentFunc, opsFunc, stmtMajorityCommittedFunc, validateFunc);
