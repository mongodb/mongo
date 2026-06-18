/**
 * Verifies that the classic UpsertStage performs its insert through the generic query-execution
 * yield machinery (handlePlanStageYield / NEED_YIELD) instead of an in-stage writeConflictRetry
 * loop. A WriteConflictException injected during the upsert insert is retried by the PlanExecutor
 * (which releases resources and backs off, then re-drives the stage) and ultimately inserts exactly
 * one document with a stable _id.
 *
 * The case that distinguishes the new path from the old in-stage loop is the retry-limit case:
 * routing the insert through the executor makes it subject to the adaptive WriteConflict retry
 * limit (internalQueryWriteConflictRetryLimitMax), so it can throw WriteConflictRetryLimitExceeded
 * -- something the old unbounded in-stage writeConflictRetry loop could never do. The remaining
 * cases guard that the refactor preserves existing behavior: retry-to-success, _id stability across
 * retries, in-transaction surfacing of the conflict, an explain that performs no insert, and a
 * single oplog entry for a capped collection.
 *
 * Resource release (locks/tickets) during backoff is internal and not directly observable from a
 * jstest, so it is not asserted here.
 *
 * @tags: [
 *   requires_replication,
 *   # Uses the WiredTiger-specific WTWriteConflictException failpoint, which is unavailable on
 *   # other storage engines.
 *   requires_wiredtiger,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Inject many conflicts so that, even though WTWriteConflictException is a single global counter
// (and WT_OP_CHECK fires on every modifying cursor op including the oplog write), the operation
// under test reliably observes a conflict before the failpoint is exhausted.
const kWriteConflictCount = 10;

describe("upsert insert write-conflict retry", function () {
    let rst, primary, db, coll;
    const collName = "upsert_write_conflict_retry";

    before(function () {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    // Disable the periodic noop writer so its oplog writes can't consume the
                    // WTWriteConflictException failpoint firings out from under the upsert insert.
                    writePeriodicNoops: false,
                    // Disable the WCE retry limit so the test doesn't depend on the server's
                    // defaults; the retry-limit case turns it back on explicitly.
                    internalQueryWriteConflictRetryLimitMax: 0,
                    internalQueryWriteConflictRetryLimitWaitersThreshold: 0,
                },
            },
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        db = primary.getDB("test");
        coll = db[collName];
    });

    after(function () {
        rst.stopSet();
    });

    it("an update upsert retries the insert after write conflicts and inserts the document exactly once", function () {
        coll.drop();
        // Pre-create the collection so the upsert insert goes through UpsertStage's yield path
        // rather than the implicit-collection-creation retry path (which handles its own conflicts).
        assert.commandWorked(db.createCollection(collName));

        const fp = configureFailPoint(
            primary,
            "WTWriteConflictException",
            {},
            {times: kWriteConflictCount},
        );
        const res = assert.commandWorked(
            db.runCommand({
                update: collName,
                updates: [{q: {x: 3}, u: {$inc: {y: 1}}, upsert: true}],
            }),
        );
        fp.off();

        assert.eq(1, res.n, res);
        assert.eq(0, res.nModified, res);
        assert.eq(1, res.upserted.length, res);

        // Exactly one document was inserted, despite the conflicts.
        assert.eq(1, coll.find({x: 3, y: 1}).itcount(), {docs: coll.find().toArray()});
        assert.eq(1, coll.find().itcount(), {docs: coll.find().toArray()});
    });

    it("a findAndModify upsert with new:true returns the inserted document after write conflicts", function () {
        coll.drop();
        assert.commandWorked(db.createCollection(collName));

        const fp = configureFailPoint(
            primary,
            "WTWriteConflictException",
            {},
            {times: kWriteConflictCount},
        );
        const res = assert.commandWorked(
            db.runCommand({
                findAndModify: collName,
                query: {x: 7},
                update: {$set: {y: 42}},
                upsert: true,
                new: true,
            }),
        );
        fp.off();

        assert.neq(null, res.value, res);
        assert.eq(7, res.value.x, res);
        assert.eq(42, res.value.y, res);
        assert(res.lastErrorObject.upserted, res);

        // The returned _id is the one actually present -- there is no orphan from a rolled-back
        // attempt, and the generated OID did not change between retries (the produced document is
        // cached, not regenerated).
        const insertedId = res.value._id;
        assert.eq(insertedId, res.lastErrorObject.upserted, res);
        assert.eq(1, coll.find({_id: insertedId}).itcount(), {docs: coll.find().toArray()});
        assert.eq(1, coll.find().itcount(), {docs: coll.find().toArray()});
    });

    it("an upsert insert that exceeds the retry limit fails with WriteConflictRetryLimitExceeded", function () {
        coll.drop();
        assert.commandWorked(db.createCollection(collName));
        const kLimit = 5;
        // Routing the insert through the executor makes it subject to the adaptive retry limit.
        // Set the waiters threshold to 0 so adaptive scaling is disabled and the effective limit
        // is exactly kLimit; the op then aborts with WriteConflictRetryLimitExceeded after kLimit
        // consecutive conflicts -- the old unbounded in-stage writeConflictRetry loop could never
        // throw this.
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryWriteConflictRetryLimitMax: kLimit,
                internalQueryWriteConflictRetryLimitWaitersThreshold: 0,
            }),
        );
        try {
            const fp = configureFailPoint(primary, "WTWriteConflictException", {}, {times: 50});
            const res = db.runCommand({
                update: collName,
                updates: [{q: {x: 99}, u: {$set: {y: 1}}, upsert: true}],
            });
            fp.off();

            assert.commandFailedWithCode(res, ErrorCodes.WriteConflictRetryLimitExceeded);
            assert.eq(0, coll.find({x: 99}).itcount(), {docs: coll.find().toArray()});
        } finally {
            // Reset before the next case: the other cases inject more conflicts than this limit and
            // would otherwise abort.
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryWriteConflictRetryLimitMax: 0}),
            );
        }
    });

    it("an explained upsert does not perform the insert", function () {
        coll.drop();
        assert.commandWorked(db.createCollection(collName));
        // An explain skips the insert entirely, so an injected WriteConflict is never hit and
        // nothing is written.
        const fp = configureFailPoint(
            primary,
            "WTWriteConflictException",
            {},
            {times: kWriteConflictCount},
        );
        assert.commandWorked(
            db.runCommand({
                explain: {
                    update: collName,
                    updates: [{q: {x: 5}, u: {$set: {y: 1}}, upsert: true}],
                },
            }),
        );
        fp.off();

        assert.eq(0, coll.find().itcount(), {docs: coll.find().toArray()});
    });

    it("an upsert inside a transaction surfaces the write conflict instead of retrying", function () {
        coll.drop();
        // Create the collection up front so the transaction does not implicitly create it.
        assert.commandWorked(db.runCommand({create: collName}));

        const session = primary.startSession();
        const sessionDb = session.getDatabase("test");

        // Inject more than one conflict for margin against any other modifying op consuming a
        // firing; in a transaction the op surfaces the first conflict without retrying.
        const fp = configureFailPoint(
            primary,
            "WTWriteConflictException",
            {},
            {times: kWriteConflictCount},
        );
        session.startTransaction();
        const res = sessionDb.runCommand({
            update: collName,
            updates: [{q: {x: 11}, u: {$set: {y: 1}}, upsert: true}],
        });
        fp.off();

        // In a multi-document transaction the plan executor cannot auto-yield (YIELD_AUTO is
        // downgraded to INTERRUPT_ONLY), so the WriteConflictException is surfaced to the client as
        // a transient transaction error rather than retried internally.
        assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
        session.abortTransaction_forTesting();
        session.endSession();

        assert.eq(0, coll.find().itcount(), {docs: coll.find().toArray()});
    });

    it("an upsert into a capped collection retries the insert and writes exactly one oplog entry", function () {
        // Use a dedicated namespace so the oplog assertion below is not confused by inserts from
        // the other test cases (which all reuse 'collName').
        const cappedCollName = collName + "_capped";
        const cappedColl = db[cappedCollName];
        cappedColl.drop();
        assert.commandWorked(
            db.createCollection(cappedCollName, {capped: true, size: 1024 * 1024}),
        );

        const fp = configureFailPoint(
            primary,
            "WTWriteConflictException",
            {},
            {times: kWriteConflictCount},
        );
        assert.commandWorked(
            db.runCommand({
                update: cappedCollName,
                updates: [{q: {_id: 1}, u: {$set: {y: 1}}, upsert: true}],
            }),
        );
        fp.off();

        assert.eq(1, cappedColl.find({_id: 1, y: 1}).itcount(), {
            docs: cappedColl.find().toArray(),
        });

        // The rolled-back attempts reserved oplog slots that are discarded; only the committed
        // insert produces an oplog entry, and that entry is the upserted document.
        const oplog = primary.getDB("local").oplog.rs;
        const insertEntries = oplog.find({op: "i", ns: "test." + cappedCollName}).toArray();
        assert.eq(1, insertEntries.length, insertEntries);
        assert.eq(1, insertEntries[0].o._id, insertEntries);
        assert.eq(1, insertEntries[0].o.y, insertEntries);
    });
});
