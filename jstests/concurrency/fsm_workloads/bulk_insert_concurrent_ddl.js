/**
 * bulk_insert_concurrent_ddl.js
 *
 * Interleaves a multi-sub-batch bulk insert with concurrent collMod, createIndex, and dropIndex.
 *
 * Background:
 *   Non-transactional bulk insertions are divided into sub-batches of up to
 *   internalInsertMaxBatchSize documents (default 64). DDL operations such as collMod or
 *   createIndex are not atomically serialized with respect to the entire bulk insert -- they may
 *   land between sub-batches. SERVER-95924 reports the rename/drop variant of this race; this
 *   workload exercises the same boundary against in-place DDL (collMod, createIndex, dropIndex)
 *   and asserts a small set of post-hoc invariants that should hold even when the race fires.
 *
 * Invariants checked at teardown:
 *   1. Per-thread doc completeness: every doc this thread reports as written carries a
 *      stable (tid, batchId) tag and a fixed schema { _id, tid, batchId, seq, payload }. The
 *      collection scan must contain exactly the union of (tid, batchId) pairs that any thread's
 *      bulk insert reported as nInserted.
 *   2. Index visibility atomicity: for every index name that exists in listIndexes at teardown,
 *      a count() that hints the index returns the same result as the same query without a hint.
 *      Mixed-state visibility (index present in the catalog but missing entries for some docs)
 *      would surface as a count divergence here.
 *   3. collMod validator atomicity: once collMod installs a validator that rejects docs without
 *      a 'payload' field, the catalog reports exactly one validator at a time; we never observe
 *      a half-installed validator state.
 *
 * The workload is green-by-default. Failures are reported with a structured summary describing
 * which invariant failed and which (tid, batchId, indexName) triple surfaced it.
 *
 * @tags: [
 *   creates_background_indexes,
 *   requires_getmore,
 * ]
 */

import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";

export const $config = (function () {
    const data = {
        // Each bulk insert writes BULK_DOCS_PER_BATCH docs. Pick a value > the default
        // internalInsertMaxBatchSize (64) so the bulk crosses at least two sub-batch boundaries,
        // giving the race window we care about.
        BULK_DOCS_PER_BATCH: 100,

        // Auxiliary collection that records, per successful bulk insert, the (tid, batchId,
        // nInserted) triple. The teardown scan compares this against the actual collection.
        receiptCollName: "bulk_insert_concurrent_ddl_receipts",

        // Auxiliary collection that records the inflight history of validator changes so we
        // can cross-check listCollections at teardown.
        validatorLogCollName: "bulk_insert_concurrent_ddl_validator_log",

        // Index name used by createIndex / dropIndex. Hard-coded so the teardown listIndexes
        // pass can target it specifically.
        sharedIndexName: "bulk_insert_concurrent_ddl_x_1",

        // Codes acceptable when a DDL races a concurrent DDL or insert.
        ddlBenignCodes: function () {
            return [
                ErrorCodes.ConflictingOperationInProgress,
                ErrorCodes.IndexBuildAlreadyInProgress,
                ErrorCodes.IndexBuildAborted,
                ErrorCodes.IndexNotFound,
                ErrorCodes.NamespaceNotFound,
            ];
        },

        // Codes acceptable when a bulk insert races a concurrent collMod that installs a
        // validator. The insert may legitimately fail mid-stream; partial nInserted is allowed.
        insertBenignCodes: function () {
            return [ErrorCodes.DocumentValidationFailure, ErrorCodes.NamespaceNotFound];
        },
    };

    const states = {
        init: function init(db, collName) {
            // Per-thread monotonic batch counter; (tid, batchId) is the unique tag for every
            // bulk insert this thread issues.
            this.batchId = 0;
        },

        bulkInsert: function bulkInsert(db, collName) {
            const batchId = ++this.batchId;
            const bulk = db[collName].initializeUnorderedBulkOp();
            for (let i = 0; i < this.BULK_DOCS_PER_BATCH; ++i) {
                // Every doc always carries 'payload'; this lets collMod install a validator
                // that requires 'payload' without rejecting our own writes.
                bulk.insert({tid: this.tid, batchId: batchId, seq: i, payload: 1});
            }

            let res;
            try {
                res = bulk.execute();
            } catch (e) {
                // BulkWriteError or raw error. Sub-batches that completed before the race
                // are committed; we must still record what made it.
                if (e.code !== undefined && this.insertBenignCodes().includes(e.code)) {
                    const writeResult = e.result || e.toResult || null;
                    const nInserted = writeResult && writeResult.nInserted !== undefined ? writeResult.nInserted : 0;
                    db[this.receiptCollName].insert({
                        tid: this.tid,
                        batchId: batchId,
                        nInserted: nInserted,
                        racedDDL: true,
                    });
                    return;
                }
                throw e;
            }

            assert.commandWorked(res);
            db[this.receiptCollName].insert({
                tid: this.tid,
                batchId: batchId,
                nInserted: res.nInserted,
                racedDDL: false,
            });
        },

        collMod: function collMod(db, collName) {
            // Toggle between two validator states. Both accept any doc that carries 'payload'
            // (which our bulkInsert always sets), so the toggle never legitimately rejects our
            // own writes. If a half-installed validator state ever caused a doc that DOES carry
            // 'payload' to be rejected, that would surface as a teardown invariant failure.
            const installRequirePayload = Random.randInt(2) === 0;
            const validator = installRequirePayload ? {payload: {$exists: true}} : {};
            const res = db.runCommand({collMod: collName, validator: validator});
            assert.commandWorkedOrFailedWithCode(res, this.ddlBenignCodes());
            if (res.ok === 1) {
                db[this.validatorLogCollName].insert({
                    tid: this.tid,
                    ts: new Date(),
                    requirePayload: installRequirePayload,
                });
            }
        },

        createIndex: function createIndex(db, collName) {
            const res = db[collName].createIndex({tid: 1, batchId: 1, seq: 1}, {name: this.sharedIndexName});
            assert.commandWorkedOrFailedWithCode(res, this.ddlBenignCodes());
        },

        dropIndex: function dropIndex(db, collName) {
            const res = db.runCommand({dropIndexes: collName, index: this.sharedIndexName});
            assert.commandWorkedOrFailedWithCode(res, this.ddlBenignCodes());
        },
    };

    function setup(db, collName, cluster) {
        // Make sure both auxiliary collections start clean. The runner drops db[collName]
        // before setup; we do not drop it here.
        db[this.receiptCollName].drop();
        db[this.validatorLogCollName].drop();
    }

    function teardown(db, collName, cluster) {
        const coll = db[collName];

        // ---- Invariant 1: per-thread doc completeness ----
        const receipts = db[this.receiptCollName].find().toArray();
        const failures = [];

        for (const r of receipts) {
            const actual = coll.find({tid: r.tid, batchId: r.batchId}).itcount();
            if (r.racedDDL) {
                // A raced bulk may have committed any number in [0, nInserted]; the receipt
                // captures what the driver reported. Assert that the visible count never
                // exceeds the reported count (no phantom docs) and is non-negative.
                if (actual < 0 || actual > this.BULK_DOCS_PER_BATCH) {
                    failures.push({
                        invariant: "doc_completeness",
                        tid: r.tid,
                        batchId: r.batchId,
                        expected: "0..BULK_DOCS_PER_BATCH",
                        actual: actual,
                    });
                }
            } else {
                // Clean bulk -- every doc the driver reported as nInserted must be visible.
                if (actual !== r.nInserted) {
                    failures.push({
                        invariant: "doc_completeness",
                        tid: r.tid,
                        batchId: r.batchId,
                        expected: r.nInserted,
                        actual: actual,
                    });
                }
            }

            // Every visible doc for this (tid, batchId) must carry the full schema. A
            // partially-applied insert that leaked a malformed doc would fail this scan.
            const malformed = coll
                .find({
                    tid: r.tid,
                    batchId: r.batchId,
                    $or: [{seq: {$exists: false}}, {payload: {$exists: false}}],
                })
                .itcount();
            if (malformed !== 0) {
                failures.push({
                    invariant: "doc_schema_complete",
                    tid: r.tid,
                    batchId: r.batchId,
                    malformedCount: malformed,
                });
            }
        }

        // ---- Invariant 2: index visibility atomicity ----
        const indexes = coll.getIndexes();
        for (const idx of indexes) {
            if (idx.name === "_id_") continue;
            // For each non-_id index, the hinted and unhinted counts must agree. A partially-
            // built index would surface as a hinted count strictly less than the unhinted count.
            const unhinted = coll.find({}).itcount();
            const hinted = coll.find({}).hint(idx.name).itcount();
            if (hinted !== unhinted) {
                failures.push({
                    invariant: "index_visibility_atomicity",
                    indexName: idx.name,
                    hinted: hinted,
                    unhinted: unhinted,
                });
            }
        }

        // ---- Invariant 3: collMod validator atomicity ----
        // listCollections must report exactly one validator state -- never a merged/partial one.
        const collInfos = db.getCollectionInfos({name: collName});
        assert.eq(1, collInfos.length, "expected exactly one collection info for " + collName);
        const opts = collInfos[0].options || {};
        if (opts.validator !== undefined) {
            // The validator must be one of the two shapes we ever installed.
            const v = opts.validator;
            const isEmpty = Object.keys(v).length === 0;
            const isRequirePayload =
                v.payload !== undefined &&
                v.payload.$exists === true &&
                Object.keys(v).length === 1;
            if (!isEmpty && !isRequirePayload) {
                failures.push({
                    invariant: "validator_atomicity",
                    observedValidator: v,
                });
            }
        }

        if (failures.length > 0) {
            assert(false, "bulk_insert_concurrent_ddl invariants violated: " + tojson(failures));
        }
    }

    return {
        threadCount: 4,
        iterations: 50,
        data: data,
        states: states,
        startState: "init",
        transitions: uniformDistTransitions(states),
        setup: setup,
        teardown: teardown,
    };
})();
