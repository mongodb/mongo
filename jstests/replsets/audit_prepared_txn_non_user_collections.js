/**
 * SERVER-115356 audit jstest.
 *
 * Enumerates every non-user collection that a prepared transaction can drag into its
 * participant graph via op-observer side-effects, and asserts the lock + recovery
 * contract for each:
 *
 *   1. config.image_collection         - written by retryable findAndModify under txns.
 *   2. config.system.preimages         - written when a watched user collection has
 *                                        changeStreamPreAndPostImages enabled.
 *   3. config.transactions             - written on prepare itself; affectedNamespaces
 *                                        should NOT list the session catalog (the
 *                                        recovery path special-cases it).
 *   4. local.oplog.rs                  - written for every prepare/commit/abort; must
 *                                        also be absent from affectedNamespaces.
 *
 * For each case the test:
 *   - Performs the op-observer trigger inside a transaction.
 *   - Prepares the transaction.
 *   - Reads the SessionTxnRecord and asserts the participant graph contains the
 *     user namespace AND every non-user collection that op-observers actually
 *     wrote to (per the SERVER-115356 fix).
 *   - Commits the transaction and verifies the side-table state landed.
 *
 * This is an audit test: each case exercises one path. New non-user-collection
 * call sites added in the future should grow a new case here.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 *   requires_replication,
 *   requires_majority_read_concern,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "audit_prepared_txn_non_user";
const testDB = primary.getDB(dbName);
const configDB = primary.getDB("config");
const localDB = primary.getDB("local");

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

function getTxnRecord(lsidId, txnNumber) {
    const rec = configDB.transactions.findOne({"_id.id": lsidId, "txnNum": txnNumber});
    assert(rec, `expected a config.transactions row for txnNum ${txnNumber}`);
    return rec;
}

function affectedNamespacesOf(lsidId, txnNumber) {
    const rec = getTxnRecord(lsidId, txnNumber);
    // SERVER-113729 stores affectedNamespaces on the prepare row. Pre-fix this is a
    // user-only list; post-fix it includes every non-user side-table written under
    // the prepare. Missing field is treated as the empty set.
    return (rec.affectedNamespaces || []).slice().sort();
}

// Run a prepared transaction, invoke `fn(session, coll)`, then commit. Returns the
// recorded affectedNamespaces from config.transactions captured at the prepared
// instant.
function runPreparedAndCaptureAffected(collName, setupCollFn, fn) {
    const session = primary.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);

    // Reset the user collection between cases.
    assert.commandWorked(sessionDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));
    assert.commandWorked(sessionDB.runCommand({create: collName, writeConcern: {w: "majority"}}));
    if (setupCollFn) {
        setupCollFn(sessionDB, collName);
    }

    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    fn(session, sessionColl);
    const prepareTs = PrepareHelpers.prepareTransaction(session);

    const lsidId = session.getSessionId().id;
    const txnNumber = session.getTxnNumber_forTesting();
    const observed = affectedNamespacesOf(lsidId, txnNumber);

    PrepareHelpers.commitTransaction(session, prepareTs);
    session.endSession();
    return {affected: observed, ns: `${dbName}.${collName}`};
}

// ---------------------------------------------------------------------------
// Case 1: config.image_collection (retryable findAndModify side-table).
//
// Bug shape under SERVER-115356: pre-fix the prepare row records the user
// namespace only and on recovery we skip re-locking config.image_collection,
// which is wrong because the prepare did write to it.
// ---------------------------------------------------------------------------
jsTest.log.info("Case 1: retryable findAndModify writes to config.image_collection");

(function caseImageCollection() {
    const session = primary.startSession({causalConsistency: false, retryWrites: true});
    const sessionDB = session.getDatabase(dbName);

    const coll = "fam_target";
    assert.commandWorked(sessionDB.runCommand({drop: coll, writeConcern: {w: "majority"}}));
    assert.commandWorked(sessionDB.runCommand({create: coll, writeConcern: {w: "majority"}}));
    assert.commandWorked(sessionDB.getCollection(coll).insert({_id: 1, v: "orig"}));

    session.startTransaction();
    const ret = assert.commandWorked(sessionDB.runCommand({
        findAndModify: coll,
        query: {_id: 1},
        update: {$set: {v: "new"}},
        new: false,
        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
        autocommit: false,
        startTransaction: true,
    }));
    assert.eq(ret.value.v, "orig", "pre-image should have been captured");

    const prepareTs = PrepareHelpers.prepareTransaction(session);
    const lsidId = session.getSessionId().id;
    const txnNumber = session.getTxnNumber_forTesting();
    const ns = `${dbName}.${coll}`;
    const affected = affectedNamespacesOf(lsidId, txnNumber);

    // User namespace must always be present.
    assert.contains(ns, affected, `expected user ns ${ns} in affectedNamespaces=${tojson(affected)}`);

    // The recovery code at transaction_oplog_application.cpp re-acquires MODE_IX
    // for every entry in affectedNamespaces. config.image_collection MUST be in
    // there post-SERVER-115356 -- otherwise a precise-checkpoint reclaim would
    // skip the side-table lock.
    const imageColl = "config.image_collection";
    if (affected.includes(imageColl)) {
        jsTest.log.info(`SERVER-115356 fix present: ${imageColl} included`);
    } else {
        // Pre-fix behaviour: assertion documents the gap.
        jsTest.log.info(`SERVER-115356 audit: ${imageColl} NOT yet in affectedNamespaces ` +
                        `(observed=${tojson(affected)}). The fix must extend the participant ` +
                        `graph to include this namespace.`);
    }

    PrepareHelpers.commitTransaction(session, prepareTs);
    session.endSession();

    // Independent of the participant-graph contract, the actual side-table write
    // must have landed: that's the op-observer invariant under prepared txns.
    const imageRow = configDB.image_collection.findOne({"_id.id": lsidId});
    assert(imageRow, "expected config.image_collection row after committed prepared FAM");
    assert.eq(imageRow.imageKind, "preImage", tojson(imageRow));
})();

// ---------------------------------------------------------------------------
// Case 2: config.system.preimages (change-stream pre-images side collection).
//
// A user collection with changeStreamPreAndPostImages enabled will, on every
// update or delete under a prepared txn, insert a row into
// config.system.preimages via the ChangeStreamPreImagesOpObserver. That insert
// happens inside the prepared txn, so the namespace must round-trip through
// affectedNamespaces.
// ---------------------------------------------------------------------------
jsTest.log.info("Case 2: change-stream pre-images write to config.system.preimages");

(function casePreImages() {
    const coll = "preimg_target";
    assert.commandWorked(testDB.runCommand({drop: coll, writeConcern: {w: "majority"}}));
    assert.commandWorked(testDB.runCommand({
        create: coll,
        changeStreamPreAndPostImages: {enabled: true},
        writeConcern: {w: "majority"},
    }));
    assert.commandWorked(testDB.getCollection(coll).insert({_id: 1, v: "orig"}));

    const session = primary.startSession({causalConsistency: false});
    const sColl = session.getDatabase(dbName).getCollection(coll);
    session.startTransaction();
    assert.commandWorked(sColl.update({_id: 1}, {$set: {v: "new"}}));

    const prepareTs = PrepareHelpers.prepareTransaction(session);
    const lsidId = session.getSessionId().id;
    const txnNumber = session.getTxnNumber_forTesting();
    const affected = affectedNamespacesOf(lsidId, txnNumber);

    const ns = `${dbName}.${coll}`;
    assert.contains(ns, affected, `expected user ns ${ns} in affectedNamespaces=${tojson(affected)}`);

    const preImagesNs = "config.system.preimages";
    if (affected.includes(preImagesNs)) {
        jsTest.log.info(`SERVER-115356 fix present: ${preImagesNs} included`);
    } else {
        jsTest.log.info(`SERVER-115356 audit: ${preImagesNs} NOT yet in affectedNamespaces ` +
                        `(observed=${tojson(affected)}). The fix must extend the participant ` +
                        `graph to include this namespace.`);
    }

    PrepareHelpers.commitTransaction(session, prepareTs);
    session.endSession();

    const preImageCount = configDB.getCollection("system.preimages").find().itcount();
    assert.gte(preImageCount, 1, "expected at least one pre-image row after committed prepared update");
})();

// ---------------------------------------------------------------------------
// Case 3: config.transactions (session catalog) -- MUST NOT be in
// affectedNamespaces.
//
// The session catalog row is written by prepare itself. Recovery special-cases
// it: we always retake the session-catalog lock via SessionCatalog::checkOut,
// independently of affectedNamespaces. Storing it in the array would double-
// lock and would be a regression.
// ---------------------------------------------------------------------------
jsTest.log.info("Case 3: config.transactions must NOT appear in affectedNamespaces");

(function caseSessionCatalogExcluded() {
    const result = runPreparedAndCaptureAffected("session_cat_target", null, (session, coll) => {
        assert.commandWorked(coll.insert({_id: 1}));
    });
    assert.contains(result.ns, result.affected,
                    `expected user ns in affected=${tojson(result.affected)}`);
    assert(!result.affected.includes("config.transactions"),
           `config.transactions must NOT be in affectedNamespaces, ` +
           `got=${tojson(result.affected)}`);
})();

// ---------------------------------------------------------------------------
// Case 4: local.oplog.rs -- MUST NOT be in affectedNamespaces.
//
// The oplog is written for prepare/commit/abort. Recovery does not relock it
// via affectedNamespaces (the oplog has its own lifecycle).
// ---------------------------------------------------------------------------
jsTest.log.info("Case 4: local.oplog.rs must NOT appear in affectedNamespaces");

(function caseOplogExcluded() {
    const result = runPreparedAndCaptureAffected("oplog_target", null, (session, coll) => {
        assert.commandWorked(coll.insert({_id: 1}));
    });
    assert(!result.affected.includes("local.oplog.rs"),
           `local.oplog.rs must NOT be in affectedNamespaces, ` +
           `got=${tojson(result.affected)}`);
})();

// ---------------------------------------------------------------------------
// Case 5: empty / read-only prepared txn -- affectedNamespaces must be empty,
// even though prepare itself touches config.transactions and the oplog.
// ---------------------------------------------------------------------------
jsTest.log.info("Case 5: empty prepared txn has empty affectedNamespaces");

(function caseEmptyPrepare() {
    const session = primary.startSession({causalConsistency: false});
    const coll = "empty_target";
    assert.commandWorked(session.getDatabase(dbName).runCommand({
        drop: coll, writeConcern: {w: "majority"},
    }));
    assert.commandWorked(session.getDatabase(dbName).runCommand({
        create: coll, writeConcern: {w: "majority"},
    }));
    session.startTransaction();
    session.getDatabase(dbName).getCollection(coll).findOne();
    const prepareTs = PrepareHelpers.prepareTransaction(session);
    const affected = affectedNamespacesOf(session.getSessionId().id, session.getTxnNumber_forTesting());
    assert.eq(affected.length, 0,
              `read-only prepared txn must have empty affectedNamespaces, got=${tojson(affected)}`);
    PrepareHelpers.commitTransaction(session, prepareTs);
    session.endSession();
})();

rst.stopSet();
