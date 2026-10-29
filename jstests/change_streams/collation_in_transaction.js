/**
 * Tests that change streams running over multi-statement transactions correctly handle collections
 * with custom collations. A user-specified collation must apply to the change events generated for
 * operations extracted from a transaction, but it must never apply to the scan over the oplog. In
 * particular, the namespace matching that filters transaction operations to the watched collection
 * must remain case-sensitive, regardless of the pipeline's collation. This exercises the
 * transaction unwind code path in both replica set and sharded cluster deployments.
 *
 * @tags: [
 *   uses_change_streams,
 *   uses_transactions,
 *   requires_majority_read_concern,
 *   requires_snapshot_read,
 *   do_not_run_in_whole_db_passthrough,
 *   do_not_run_in_whole_cluster_passthrough,
 * ]
 */
import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

describe("change streams over multi-statement transactions with collations", function () {
    const caseInsensitive = {locale: "en_US", strength: 2};

    const sessionOptions = {causalConsistency: false};
    const txnOptions = {readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}};

    const kWatchedCollName = "change_stream_txn_case_insensitive";
    // A collection whose name only differs in case from the watched collection.
    const kSimilarCollName = "cHaNgE_sTrEaM_tXn_cAsE_iNsEnSiTiVe";

    const caseInsensitivePipeline = [
        {$changeStream: {}},
        {$match: {"fullDocument.text": "abc"}},
        {$project: {docId: "$documentKey._id"}},
    ];

    let cst;
    let session;
    let sessionDb;

    before(function () {
        // Create the collections that the transactions will write to. Both use a case-insensitive
        // default collation, and their names differ only in case. The collections must exist before
        // the transactions run, because collections cannot be created (or implicitly sharded)
        // inside a multi-statement transaction.
        assertDropAndRecreateCollection(db, kWatchedCollName, {collation: caseInsensitive});
        assertDropAndRecreateCollection(db, kSimilarCollName, {collation: caseInsensitive});

        session = db.getMongo().startSession(sessionOptions);
        sessionDb = session.getDatabase(db.getName());
    });

    beforeEach(function () {
        cst = new ChangeStreamTest(db);
    });

    afterEach(function () {
        cst.cleanUp();
    });

    after(function () {
        session.endSession();
        assertDropCollection(db, kWatchedCollName);
        assertDropCollection(db, kSimilarCollName);
    });

    it("applies an explicit collation to change events generated from a transaction", function () {
        const stream = cst.startWatchingChanges({
            pipeline: caseInsensitivePipeline,
            collection: kWatchedCollName,
            aggregateOptions: {collation: caseInsensitive},
        });

        const sessionColl = sessionDb[kWatchedCollName];
        withTxnAndAutoRetryOnMongos(
            session,
            () => {
                assert.commandWorked(sessionColl.insert({_id: 0, text: "aBc"}));
                assert.commandWorked(sessionColl.insert({_id: 1, text: "abc"}));
            },
            txnOptions,
        );

        // With the case-insensitive collation, the user $match on 'fullDocument.text' matches both
        // "aBc" and "abc".
        cst.assertNextChangesEqual({cursor: stream, expectedChanges: [{docId: 0}, {docId: 1}]});
    });

    it("does not inherit the collection's default collation for change events", function () {
        // Open the stream without an explicit collation, so it uses the simple collation and does
        // not implicitly adopt the collection's case-insensitive default collation. Tagged
        // 'doNotModifyInPassthroughs' because only single-collection streams have the concept of a
        // default collation.
        const stream = cst.startWatchingChanges({
            pipeline: caseInsensitivePipeline,
            collection: kWatchedCollName,
            doNotModifyInPassthroughs: true,
        });

        const sessionColl = sessionDb[kWatchedCollName];
        withTxnAndAutoRetryOnMongos(
            session,
            () => {
                assert.commandWorked(sessionColl.insert({_id: 2, text: "aBc"}));
                assert.commandWorked(sessionColl.insert({_id: 3, text: "abc"}));
            },
            txnOptions,
        );

        // With the simple collation, the user $match matches only the exact "abc".
        cst.assertNextChangesEqual({cursor: stream, expectedChanges: [{docId: 3}]});
    });

    it("does not apply the collation to the transaction namespace filter", function () {
        // The change stream watches only 'kWatchedCollName' with an explicit case-insensitive
        // collation. Even though 'kSimilarCollName' differs only in case (and would compare equal
        // under the case-insensitive collation), the namespace filter applied to the transaction
        // operations must remain case-sensitive, so events from 'kSimilarCollName' must not appear.
        // Tagged 'doNotModifyInPassthroughs' so that the stream is not rewritten to a whole-db or
        // whole-cluster stream, which would not filter the oplog by collection name.
        const stream = cst.startWatchingChanges({
            pipeline: caseInsensitivePipeline,
            collection: kWatchedCollName,
            aggregateOptions: {collation: caseInsensitive},
            doNotModifyInPassthroughs: true,
        });

        // A single transaction writes to both collections. When the transaction is unwound, the
        // write to the similarly-named collection must be rejected by the namespace filter, and
        // only the write to the watched collection must produce a change event.
        const sessionSimilarColl = sessionDb[kSimilarCollName];
        const sessionWatchedColl = sessionDb[kWatchedCollName];
        withTxnAndAutoRetryOnMongos(
            session,
            () => {
                assert.commandWorked(sessionSimilarColl.insert({_id: 10, text: "aBc"}));
                assert.commandWorked(sessionWatchedColl.insert({_id: 20, text: "ABC"}));
            },
            txnOptions,
        );

        // Only the insert into the watched collection is returned.
        cst.assertNextChangesEqual({cursor: stream, expectedChanges: [{docId: 20}]});
    });
});
