/**
 * Tests that the rollback procedure will update the 'config.transactions' table to be consistent
 * with the node data at the 'stableTimestamp', specifically in the case where multiple insert ops
 * to the 'config.transactions' table were coalesced into a single operation during secondary oplog
 * application.
 *
 * @tags: [requires_persistence]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    runTests
} from
    "jstests/replsets/libs/rollback_with_coalesced_txn_table_updates_during_oplog_application_helper.js";

const initFunc = (primary, ns, counterTotal) => {
    if (FeatureFlagUtil.isPresentAndEnabled(primary, "ReplicateVectoredInsertsTransactionally")) {
        // Set the batch size to 2 so we're testing batching but don't have to insert a huge number
        // of documents
        assert.commandWorked(
            primary.adminCommand({setParameter: 1, internalInsertMaxBatchSize: 2}));
    }
};

const stopReplProducerOnDocumentFunc = (counterMajorityCommitted) => {
    return {document: {"_id": counterMajorityCommitted + 1}};
};

const opsFunc = (primary, ns, counterTotal, lsid) => {
    assert.commandWorked(primary.getCollection(ns).runCommand("insert", {
        documents: Array.from({length: counterTotal}, (_, i) => ({_id: i})),
        lsid,
        txnNumber: NumberLong(2),
    }));
};

const stmtMajorityCommittedFunc = (primary, ns, counterMajorityCommitted) => {
    if (FeatureFlagUtil.isPresentAndEnabled(primary, "ReplicateVectoredInsertsTransactionally")) {
        return {"o.applyOps.ns": ns, "o.applyOps.o._id": counterMajorityCommitted};
    } else {
        return {ns: ns, "o._id": counterMajorityCommitted};
    }
};

const validateFunc = (secondary1, ns, counterMajorityCommitted, counterTotal, lsid) => {
    // Make sure we don't re-execute operations that have already been inserted by making sure we
    // we don't get a 'DuplicateKeyError'.
    assert.commandWorked(secondary1.getCollection(ns).runCommand("insert", {
        documents: Array.from({length: counterTotal}, (_, i) => ({_id: i})),
        lsid,
        txnNumber: NumberLong(2),
        writeConcern: {w: 5},
    }));
};

runTests(
    initFunc, stopReplProducerOnDocumentFunc, opsFunc, stmtMajorityCommittedFunc, validateFunc);
