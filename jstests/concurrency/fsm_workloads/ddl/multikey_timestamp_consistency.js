/**
 * Tests timestamp consistency of multikey metadata across replicas under concurrent load.
 *
 * Several kinds of multikey-triggering operations run concurrently:
 *
 * Transactional (expected to be timestamp-consistent):
 * 1. createCollAndInsertWildcardMkInTxn: Creates a NEW collection + wildcard index + multikey
 *    insert in one transaction. Exercises the path where a side txn can't see the index.
 * 2. createCollAndInsertRegularMkInTxn: Same but with a regular {a: 1} index.
 * 3. createCollAndUpdateWildcardMkInTxn: Creates a NEW collection + wildcard index + scalar
 *    insert + update-to-array in one transaction. Exercises the update-path multikey trigger
 *    when the index was created in the same txn (side txn can't see the index).
 * 4. createCollAndUpdateRegularMkInTxn: Same but with a regular {a: 1} index.
 * 5. insertWildcardMkInTxn: N (1-5) multikey inserts on distinct fields (a shuffled subset of
 *    INDEXED_FIELDS) into a pre-existing wildcard-indexed collection in a single transaction.
 *    Exercises both single-statement and multi-statement side-write accumulation across
 *    distinct multikey paths.
 * 6. insertRegularMkInTxn: N (1-5) multikey inserts into a pre-existing regular-indexed
 *    collection that has a separate {field: 1} index per field in INDEXED_FIELDS. Each
 *    statement targets a distinct field so each insert exercises a distinct index's multikey
 *    path.
 * 7. updateWildcardMkInTxn: N (1-5) updates of pre-inserted scalar fields to arrays inside a
 *    transaction against a pre-existing wildcard-indexed collection. Exercises the update-path
 *    multikey transition across distinct paths.
 * 8. updateRegularMkInTxn: N (1-5) updates of pre-inserted scalars to arrays inside a
 *    transaction against the same multi-index regular collection as state 6, with each
 *    statement targeting a distinct field.
 *
 * Transactional states 5-8 randomly prepare the transaction before commit with probability
 * `prepareProbability`, exercising both unprepared and prepared commit paths.
 *
 * Non-transactional (expected to be timestamp-consistent):
 * 9. insertWildcardMkDirect: N (1-5) non-transactional multikey inserts on distinct fields into
 *    a pre-existing wildcard-indexed collection. Adjacent inserts may be batched together
 *    during oplog application on the secondary, exercising batched multikey-metadata apply.
 * 10. insertRegularMkDirect: N (1-5) non-transactional multikey inserts into a pre-existing
 *     regular-indexed collection that has a separate {field: 1} index per field in
 *     INDEXED_FIELDS, with each insert targeting a distinct field. Adjacent inserts may
 *     be batched together during oplog application on the secondary.
 * 11. updateWildcardMkDirect: N (1-5) non-transactional updates of pre-inserted scalar fields
 *     to arrays against a pre-existing wildcard-indexed collection. Adjacent updates may be
 *     batched together during oplog application on the secondary.
 * 12. updateRegularMkDirect: N (1-5) non-transactional updates of pre-inserted scalars to
 *     arrays against the same multi-index regular collection as state 10. Adjacent updates may
 *     be batched together during oplog application on the secondary.
 * 13. dropAndRecreateIndex: Non-transactionally drops a random index (wildcard or one of the
 *     regular per-field indexes) on a random pool collection and immediately recreates it.
 *     Stresses the interaction between index lifecycle and concurrent multikey side-writes
 *     from other threads. Tolerates IndexNotFound (another thread may have dropped first).
 * 14. dropAndRecreateColl: Non-transactionally drops a random pool collection and recreates it
 *     with the full set of wildcard + regular indexes. Stresses catalog churn against
 *     concurrent multikey side-writes. Other pool-using states tolerate NamespaceNotFound and
 *     CollectionUUIDMismatch via isCollDropError when their writes race with this state.
 *
 * 15. insertNoise: Non-transactional insert to an unrelated collection for batch pressure.
 *
 * After each operation, the thread verifies that the catalog multikey flag (and index-specific
 * multikey paths) are consistent between primary and secondary at the operation's timestamp
 * and the preceding timestamp.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   uses_transactions,
 *   featureFlagReplicateMultikeynessInTransactions,
 *   requires_replication,
 *   does_not_support_stepdowns,
 *   # Suites converting to txns would break our assumption about oplog format of inserts.
 *   does_not_support_transactions,
 *   # The workload's internal contention plus a per-test runtime in the tens of minutes exceeds
 *   # the burn-in budget when layered under simultaneous FSM workloads.
 *   incompatible_with_concurrency_simultaneous,
 * ]
 */

import {withTxnAndAutoRetry} from "jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js";
import {
    findIndexForField,
    IndexType,
    readCatalogIndexesAtClusterTime,
    readWildcardMultikeyPaths,
} from "jstests/libs/multikey_consistency_check.js";
import {PersistenceProviderUtil} from "jstests/libs/server-rss/persistence_provider_util.js";

export const $config = (function () {
    const collPrefix = "mk_ts_";

    function poolCollName(idx) {
        return collPrefix + "pool_" + idx;
    }
    function pickPoolColl() {
        return poolCollName(Random.randInt(POOL_SIZE));
    }

    // Returns true if `e` is a transient error caused by a concurrent dropAndRecreateColl on
    // the target collection. Callers tolerate it by returning early without verification.
    //   NamespaceNotFound: a non-txn write hit the post-drop, pre-recreate window.
    //   CollectionUUIDMismatch: a non-txn write referenced the pre-drop UUID after recreate.
    //   OperationNotSupportedInTransaction: an in-txn write implicitly created the dropped
    //     collection, then prepare/commit rejected because distributed txns disallow implicit
    //     collection creation.
    //   QueryPlanKilled: an in-flight plan executor (e.g. EXPRESS_UPDATE) was killed when the
    //     collection was dropped underneath it.
    function isCollDropError(e) {
        const tolerated = [
            ErrorCodes.NamespaceNotFound,
            ErrorCodes.CollectionUUIDMismatch,
            ErrorCodes.OperationNotSupportedInTransaction,
            ErrorCodes.QueryPlanKilled,
        ];
        // A non-txn write that races a drop returns ok:1 at the command level with the real
        // failure nested in writeErrors (e.g. QueryPlanKilled when the plan executor is killed
        // mid-update), so e.code is undefined and only the writeError carries the code. When
        // writeErrors are present they are the authoritative source: tolerate iff every nested
        // write error is a coll-drop race (a single non-tolerated error makes the whole thing
        // non-tolerable). Only when there are no writeErrors do we check the top-level e.code.
        if (Array.isArray(e.writeErrors) && e.writeErrors.length > 0) {
            return e.writeErrors.every((we) => tolerated.includes(we.code));
        }
        return tolerated.includes(e.code);
    }

    // Returns true and best-effort aborts any leftover transaction on the session if `e` is a
    // tolerable coll-drop race; returns false otherwise. withTxnAndAutoRetry skips the abort
    // path when an error is raised by prepare/commit (treated as a commit error of unknown
    // outcome), which leaves the session with an active transaction marker. Aborting here
    // guarantees the next state can start a fresh transaction. NoSuchTransaction is expected
    // when there was no leftover transaction (e.g. for non-txn states).
    function tolerateCollDropError(state, e) {
        if (!isCollDropError(e)) return false;
        assert.commandWorkedOrFailedWithCode(
            state.session.abortTransaction_forTesting(),
            ErrorCodes.NoSuchTransaction,
        );
        return true;
    }
    // Number of pre-existing collections in the shared pool. Each pool collection has both a
    // wildcard {"$**": 1} index and one {field: 1} regular index per field in
    // REGULAR_INDEX_FIELDS. States pick a random pool collection on each invocation so threads
    // contend on the same multikey metadata, exercising concurrent setMultikey paths.
    const POOL_SIZE = 2;

    // Fields exercised by regular-index states. Each gets its own {field: 1} index on every
    // pool collection.
    const REGULAR_INDEX_FIELDS = ["a", "b", "c", "d", "e"];

    // Fields exercised by wildcard-index states. Overlap with REGULAR_INDEX_FIELDS on "d" and
    // "e" so a single op can trigger multikey transitions on both index types simultaneously.
    // The wildcard {"$**": 1} index physically covers any field, so f/g/h hit only the
    // wildcard index while a/b/c hit only the regular indexes.
    const WILDCARD_FIELDS = ["d", "e", "f", "g", "h"];

    // --- Shared helpers ---

    const OPLOG_SCAN_LOST_MSG =
        "Skipping multikey consistency check: oplog scan lost capped position (concurrent oplog rotation)";

    // Returns the applyOps oplog entry whose `o.applyOps` array contains an element matching
    // `elemMatch`, or null if the entry is absent (e.g. the txn committed with a no-op update
    // because dropAndRecreateColl removed its target doc) or the oplog got truncated
    // (CappedPositionLost). Callers handle null by skipping verification. The outer `op: "c"`
    // restricts the match to applyOps txn entries and excludes any standalone CRUD entry.
    function findTxnOplogEntry(primary, elemMatch) {
        try {
            return primary.getDB("local").oplog.rs.findOne({
                op: "c",
                "o.applyOps": {$elemMatch: elemMatch},
            });
        } catch (e) {
            if (e.code === ErrorCodes.CappedPositionLost) {
                jsTestLog(OPLOG_SCAN_LOST_MSG + ": " + tojson(e));
                return null;
            }
            throw e;
        }
    }

    // Returns the oplog entry matching `match`, or null if the entry is absent (e.g. the op
    // was a no-op update because dropAndRecreateColl removed its target doc) or the oplog got
    // truncated (CappedPositionLost). Callers handle null by skipping verification.
    function findOplogEntry(primary, match) {
        try {
            return primary.getDB("local").oplog.rs.findOne(match);
        } catch (e) {
            if (e.code === ErrorCodes.CappedPositionLost) {
                jsTestLog(OPLOG_SCAN_LOST_MSG + ": " + tojson(e));
                return null;
            }
            throw e;
        }
    }

    // Returns the timestamp of the most recent oplog entry strictly before `ts`, or null if the
    // oplog truncated (CappedPositionLost).
    function findPrevTimestamp(primary, ts) {
        let prev;
        try {
            prev = primary
                .getDB("local")
                .oplog.rs.find({ts: {$lt: ts}})
                .sort({ts: -1})
                .limit(1)
                .toArray()[0];
            assert(prev, "Could not find a previous oplog entry before ts: " + tojson(ts));
        } catch (e) {
            if (e.code === ErrorCodes.CappedPositionLost) {
                jsTestLog(OPLOG_SCAN_LOST_MSG + ": " + tojson(e));
                return null;
            }
            throw e;
        }
        return prev ? prev.ts : null;
    }

    // Checks multikey metadata consistency between primary and secondary at two timestamps.
    function verifyMultikeyConsistency(
        primary,
        secondary,
        dbName,
        collName,
        indexType,
        wildcardHint,
        fieldNames,
        ts1,
        ts2,
    ) {
        for (const ts of [ts1, ts2]) {
            const pIndexes = readCatalogIndexesAtClusterTime(primary, dbName, collName, ts);
            const sIndexes = readCatalogIndexesAtClusterTime(secondary, dbName, collName, ts);
            if (pIndexes === undefined || sIndexes === undefined) continue;

            // Both must agree on collection existence.
            if (!pIndexes || !sIndexes) {
                assert.eq(
                    !pIndexes,
                    !sIndexes,
                    "Catalog entry presence mismatch at ts " + tojson(ts) + " for " + collName,
                );
                continue;
            }

            // Compare per-index multikey flag and paths for each field.
            for (const fieldName of fieldNames) {
                const pIdx = findIndexForField(pIndexes, indexType, fieldName);
                const sIdx = findIndexForField(sIndexes, indexType, fieldName);
                // Index may not exist at this ts yet (e.g., prevTs before createIndex). Skip.
                if (!pIdx || !sIdx) continue;

                if (pIdx.multikey !== sIdx.multikey) {
                    assert.eq(
                        pIdx.multikey,
                        sIdx.multikey,
                        "Index multikey flag mismatch at ts " +
                            tojson(ts) +
                            " for " +
                            collName +
                            " field " +
                            fieldName,
                    );
                    continue;
                }

                if (indexType === IndexType.REGULAR) {
                    const pPaths = pIdx.multikeyPaths[fieldName];
                    const sPaths = sIdx.multikeyPaths[fieldName];
                    assert.eq(
                        pPaths,
                        sPaths,
                        "Regular multikeyPaths mismatch at ts " +
                            tojson(ts) +
                            " for " +
                            collName +
                            " field " +
                            fieldName,
                    );
                } else {
                    const pPaths = readWildcardMultikeyPaths(
                        primary,
                        dbName,
                        collName,
                        ts,
                        wildcardHint,
                        fieldName,
                    );
                    const sPaths = readWildcardMultikeyPaths(
                        secondary,
                        dbName,
                        collName,
                        ts,
                        wildcardHint,
                        fieldName,
                    );
                    if (pPaths === undefined || sPaths === undefined) continue;
                    assert.sameMembers(
                        pPaths,
                        sPaths,
                        "Wildcard multiKeyPaths mismatch at ts " +
                            tojson(ts) +
                            " for " +
                            collName +
                            " field " +
                            fieldName,
                    );
                }
            }
        }
    }

    // Returns a shuffled subset of `fields` of size 1..fields.length.
    function pickFieldSubset(fields) {
        const numOps = 1 + Random.randInt(fields.length);
        return Array.shuffle(fields.slice()).slice(0, numOps);
    }

    // Returns `count` unique ids of the form `${prefix}${tid}_${insertCount}` and bumps
    // `state.insertCount` for each.
    function nextIds(state, prefix, count) {
        const ids = [];
        for (let i = 0; i < count; i++) {
            ids.push(prefix + state.tid + "_" + state.insertCount);
            state.insertCount++;
        }
        return ids;
    }

    // After a transaction commits, locates the applyOps oplog entry matching `oplogElemMatch`,
    // computes the prior timestamp, and asserts multikey consistency between primary and
    // secondary at both timestamps for the given fields.
    function verifyTxnMultikey(
        state,
        db,
        myCollName,
        oplogElemMatch,
        indexType,
        wildcardHint,
        fields,
    ) {
        const primary = db.getMongo();
        const dbName = db.getName();
        const txnEntry = findTxnOplogEntry(primary, oplogElemMatch);
        if (!txnEntry) return;
        const prevTs = findPrevTimestamp(primary, txnEntry.ts);
        if (!prevTs || !state.secondaryConn) return;

        verifyMultikeyConsistency(
            primary,
            state.secondaryConn,
            dbName,
            myCollName,
            indexType,
            wildcardHint,
            fields,
            txnEntry.ts,
            prevTs,
        );
    }

    // After a sequence of non-transactional ops, locates the first and last oplog entries
    // matching `firstMatch` / `lastMatch`, computes the prior timestamp, and asserts multikey
    // consistency between primary and secondary.
    function verifyDirectMultikey(
        state,
        db,
        myCollName,
        firstMatch,
        lastMatch,
        indexType,
        wildcardHint,
        fields,
    ) {
        const primary = db.getMongo();
        const dbName = db.getName();
        const firstEntry = findOplogEntry(primary, firstMatch);
        const lastEntry = findOplogEntry(primary, lastMatch);
        if (!firstEntry || !lastEntry) return;
        const prevTs = findPrevTimestamp(primary, firstEntry.ts);
        if (!prevTs || !state.secondaryConn) return;

        verifyMultikeyConsistency(
            primary,
            state.secondaryConn,
            dbName,
            myCollName,
            indexType,
            wildcardHint,
            fields,
            lastEntry.ts,
            prevTs,
        );
    }

    // --- FSM states ---

    let states = {
        init: function init(db, collName) {
            this.session = db.getMongo().startSession({causalConsistency: false});
            this.sessionDb = this.session.getDatabase(db.getName());
            this.insertCount = 0;
            if (this.secondaryHost) {
                this.secondaryConn = new Mongo(this.secondaryHost);
                this.secondaryConn.setSecondaryOk();
            }
        },

        // --- Transactional states (timestamp-consistent, assert on mismatch) ---

        createCollAndInsertWildcardMkInTxn: function createCollAndInsertWildcardMkInTxn(
            db,
            collName,
        ) {
            const myCollName = collPrefix + "wc_txn_" + this.tid + "_" + this.insertCount;
            this.insertCount++;

            this.session.startTransaction({writeConcern: {w: "majority"}});
            try {
                assert.commandWorked(this.sessionDb.createCollection(myCollName));
                assert.commandWorked(this.sessionDb[myCollName].createIndex({"$**": 1}));
                assert.commandWorked(this.sessionDb[myCollName].insert({a: [1, 2]}));
                assert.commandWorked(this.session.commitTransaction_forTesting());
            } catch (e) {
                this.session.abortTransaction_forTesting();
                if (
                    e.hasOwnProperty("errorLabels") &&
                    e.errorLabels.includes("TransientTransactionError")
                )
                    return;
                throw e;
            }

            const dbName = db.getName();
            verifyTxnMultikey(
                this,
                db,
                myCollName,
                {ns: dbName + "." + myCollName},
                IndexType.WILDCARD,
                {"$**": 1},
                ["a"],
            );
        },

        createCollAndInsertRegularMkInTxn: function createCollAndInsertRegularMkInTxn(
            db,
            collName,
        ) {
            const myCollName = collPrefix + "reg_txn_" + this.tid + "_" + this.insertCount;
            this.insertCount++;

            this.session.startTransaction({writeConcern: {w: "majority"}});
            try {
                assert.commandWorked(this.sessionDb.createCollection(myCollName));
                assert.commandWorked(this.sessionDb[myCollName].createIndex({a: 1}));
                assert.commandWorked(this.sessionDb[myCollName].insert({a: [1, 2]}));
                assert.commandWorked(this.session.commitTransaction_forTesting());
            } catch (e) {
                this.session.abortTransaction_forTesting();
                if (
                    e.hasOwnProperty("errorLabels") &&
                    e.errorLabels.includes("TransientTransactionError")
                )
                    return;
                throw e;
            }

            const dbName = db.getName();
            verifyTxnMultikey(
                this,
                db,
                myCollName,
                {ns: dbName + "." + myCollName},
                IndexType.REGULAR,
                null,
                ["a"],
            );
        },

        createCollAndUpdateWildcardMkInTxn: function createCollAndUpdateWildcardMkInTxn(
            db,
            collName,
        ) {
            const myCollName = collPrefix + "wc_txn_upd_" + this.tid + "_" + this.insertCount;
            this.insertCount++;

            this.session.startTransaction({writeConcern: {w: "majority"}});
            try {
                assert.commandWorked(this.sessionDb.createCollection(myCollName));
                assert.commandWorked(this.sessionDb[myCollName].createIndex({"$**": 1}));
                assert.commandWorked(this.sessionDb[myCollName].insert({_id: 1, a: 1}));
                assert.commandWorked(
                    this.sessionDb[myCollName].update({_id: 1}, {$set: {a: [1, 2]}}),
                );
                assert.commandWorked(this.session.commitTransaction_forTesting());
            } catch (e) {
                this.session.abortTransaction_forTesting();
                if (
                    e.hasOwnProperty("errorLabels") &&
                    e.errorLabels.includes("TransientTransactionError")
                )
                    return;
                throw e;
            }

            const dbName = db.getName();
            verifyTxnMultikey(
                this,
                db,
                myCollName,
                {ns: dbName + "." + myCollName},
                IndexType.WILDCARD,
                {"$**": 1},
                ["a"],
            );
        },

        createCollAndUpdateRegularMkInTxn: function createCollAndUpdateRegularMkInTxn(
            db,
            collName,
        ) {
            const myCollName = collPrefix + "reg_txn_upd_" + this.tid + "_" + this.insertCount;
            this.insertCount++;

            this.session.startTransaction({writeConcern: {w: "majority"}});
            try {
                assert.commandWorked(this.sessionDb.createCollection(myCollName));
                assert.commandWorked(this.sessionDb[myCollName].createIndex({a: 1}));
                assert.commandWorked(this.sessionDb[myCollName].insert({_id: 1, a: 1}));
                assert.commandWorked(
                    this.sessionDb[myCollName].update({_id: 1}, {$set: {a: [1, 2]}}),
                );
                assert.commandWorked(this.session.commitTransaction_forTesting());
            } catch (e) {
                this.session.abortTransaction_forTesting();
                if (
                    e.hasOwnProperty("errorLabels") &&
                    e.errorLabels.includes("TransientTransactionError")
                )
                    return;
                throw e;
            }

            const dbName = db.getName();
            verifyTxnMultikey(
                this,
                db,
                myCollName,
                {ns: dbName + "." + myCollName},
                IndexType.REGULAR,
                null,
                ["a"],
            );
        },

        insertWildcardMkInTxn: function insertWildcardMkInTxn(db, collName) {
            const myCollName = pickPoolColl();
            const fields = pickFieldSubset(WILDCARD_FIELDS);
            const markers = nextIds(this, "wc_txn_", fields.length);

            try {
                withTxnAndAutoRetry(
                    this.session,
                    () => {
                        for (let i = 0; i < fields.length; i++) {
                            assert.commandWorked(
                                this.sessionDb[myCollName].insert({
                                    [fields[i]]: [1, 2],
                                    _marker: markers[i],
                                }),
                            );
                        }
                    },
                    {
                        retryOnKilledSession: this.retryOnKilledSession,
                        prepareProbability: this.prepareProbability,
                    },
                );
            } catch (e) {
                if (tolerateCollDropError(this, e)) return;
                throw e;
            }

            const fullNs = db.getName() + "." + myCollName;
            verifyTxnMultikey(
                this,
                db,
                myCollName,
                {"op": "i", "ns": fullNs, "o._marker": markers[0]},
                IndexType.WILDCARD,
                {"$**": 1},
                fields,
            );
        },

        insertRegularMkInTxn: function insertRegularMkInTxn(db, collName) {
            const myCollName = pickPoolColl();
            const fields = pickFieldSubset(REGULAR_INDEX_FIELDS);
            const markers = nextIds(this, "r", fields.length);

            try {
                withTxnAndAutoRetry(
                    this.session,
                    () => {
                        for (let i = 0; i < fields.length; i++) {
                            assert.commandWorked(
                                this.sessionDb[myCollName].insert({
                                    [fields[i]]: [1, 2],
                                    _marker: markers[i],
                                }),
                            );
                        }
                    },
                    {
                        retryOnKilledSession: this.retryOnKilledSession,
                        prepareProbability: this.prepareProbability,
                    },
                );
            } catch (e) {
                if (tolerateCollDropError(this, e)) return;
                throw e;
            }

            const fullNs = db.getName() + "." + myCollName;
            verifyTxnMultikey(
                this,
                db,
                myCollName,
                {"op": "i", "ns": fullNs, "o._marker": markers[0]},
                IndexType.REGULAR,
                null,
                fields,
            );
        },

        updateWildcardMkInTxn: function updateWildcardMkInTxn(db, collName) {
            const myCollName = pickPoolColl();
            const fields = pickFieldSubset(WILDCARD_FIELDS);
            const docIds = nextIds(this, "u_wc_", fields.length);

            try {
                for (let i = 0; i < fields.length; i++) {
                    assert.commandWorked(db[myCollName].insert({_id: docIds[i], [fields[i]]: 1}));
                }
            } catch (e) {
                if (tolerateCollDropError(this, e)) return;
                throw e;
            }

            try {
                withTxnAndAutoRetry(
                    this.session,
                    () => {
                        for (let i = 0; i < fields.length; i++) {
                            assert.commandWorked(
                                this.sessionDb[myCollName].update(
                                    {_id: docIds[i]},
                                    {$set: {[fields[i]]: [1, 2]}},
                                ),
                            );
                        }
                    },
                    {
                        retryOnKilledSession: this.retryOnKilledSession,
                        prepareProbability: this.prepareProbability,
                    },
                );
            } catch (e) {
                if (tolerateCollDropError(this, e)) return;
                throw e;
            }

            const fullNs = db.getName() + "." + myCollName;
            verifyTxnMultikey(
                this,
                db,
                myCollName,
                {"op": "u", "ns": fullNs, "o2._id": docIds[0]},
                IndexType.WILDCARD,
                {"$**": 1},
                fields,
            );
        },

        updateRegularMkInTxn: function updateRegularMkInTxn(db, collName) {
            const myCollName = pickPoolColl();
            const fields = pickFieldSubset(REGULAR_INDEX_FIELDS);
            const docIds = nextIds(this, "u_reg_", fields.length);

            try {
                for (let i = 0; i < fields.length; i++) {
                    assert.commandWorked(db[myCollName].insert({_id: docIds[i], [fields[i]]: 1}));
                }
            } catch (e) {
                if (tolerateCollDropError(this, e)) return;
                throw e;
            }

            try {
                withTxnAndAutoRetry(
                    this.session,
                    () => {
                        for (let i = 0; i < fields.length; i++) {
                            assert.commandWorked(
                                this.sessionDb[myCollName].update(
                                    {_id: docIds[i]},
                                    {$set: {[fields[i]]: [1, 2]}},
                                ),
                            );
                        }
                    },
                    {
                        retryOnKilledSession: this.retryOnKilledSession,
                        prepareProbability: this.prepareProbability,
                    },
                );
            } catch (e) {
                if (tolerateCollDropError(this, e)) return;
                throw e;
            }

            const fullNs = db.getName() + "." + myCollName;
            verifyTxnMultikey(
                this,
                db,
                myCollName,
                {"op": "u", "ns": fullNs, "o2._id": docIds[0]},
                IndexType.REGULAR,
                null,
                fields,
            );
        },

        // --- Non-transactional states (NOT timestamp-consistent, log only) ---

        insertWildcardMkDirect: function insertWildcardMkDirect(db, collName) {
            const myCollName = pickPoolColl();
            const fields = pickFieldSubset(WILDCARD_FIELDS);
            const markers = nextIds(this, "wc_direct_", fields.length);

            try {
                for (let i = 0; i < fields.length; i++) {
                    assert.commandWorked(
                        db[myCollName].insert({[fields[i]]: [1, 2], _marker: markers[i]}),
                    );
                }
            } catch (e) {
                if (tolerateCollDropError(this, e)) return;
                throw e;
            }

            const fullNs = db.getName() + "." + myCollName;
            verifyDirectMultikey(
                this,
                db,
                myCollName,
                {"ns": fullNs, "op": "i", "o._marker": markers[0]},
                {"ns": fullNs, "op": "i", "o._marker": markers[fields.length - 1]},
                IndexType.WILDCARD,
                {"$**": 1},
                fields,
            );
        },

        insertRegularMkDirect: function insertRegularMkDirect(db, collName) {
            const myCollName = pickPoolColl();
            const fields = pickFieldSubset(REGULAR_INDEX_FIELDS);
            const markers = nextIds(this, "m", fields.length);

            try {
                for (let i = 0; i < fields.length; i++) {
                    assert.commandWorked(
                        db[myCollName].insert({[fields[i]]: [1, 2], _marker: markers[i]}),
                    );
                }
            } catch (e) {
                if (tolerateCollDropError(this, e)) return;
                throw e;
            }

            const fullNs = db.getName() + "." + myCollName;
            verifyDirectMultikey(
                this,
                db,
                myCollName,
                {"ns": fullNs, "op": "i", "o._marker": markers[0]},
                {"ns": fullNs, "op": "i", "o._marker": markers[fields.length - 1]},
                IndexType.REGULAR,
                null,
                fields,
            );
        },

        updateWildcardMkDirect: function updateWildcardMkDirect(db, collName) {
            const myCollName = pickPoolColl();
            const fields = pickFieldSubset(WILDCARD_FIELDS);
            const docIds = nextIds(this, "u_wc_direct_", fields.length);

            try {
                for (let i = 0; i < fields.length; i++) {
                    assert.commandWorked(db[myCollName].insert({_id: docIds[i], [fields[i]]: 1}));
                }
                for (let i = 0; i < fields.length; i++) {
                    assert.commandWorked(
                        db[myCollName].update({_id: docIds[i]}, {$set: {[fields[i]]: [1, 2]}}),
                    );
                }
            } catch (e) {
                if (tolerateCollDropError(this, e)) return;
                throw e;
            }

            const fullNs = db.getName() + "." + myCollName;
            verifyDirectMultikey(
                this,
                db,
                myCollName,
                {"ns": fullNs, "op": "u", "o2._id": docIds[0]},
                {"ns": fullNs, "op": "u", "o2._id": docIds[fields.length - 1]},
                IndexType.WILDCARD,
                {"$**": 1},
                fields,
            );
        },

        updateRegularMkDirect: function updateRegularMkDirect(db, collName) {
            const myCollName = pickPoolColl();
            const fields = pickFieldSubset(REGULAR_INDEX_FIELDS);
            const docIds = nextIds(this, "u_reg_direct_", fields.length);

            try {
                for (let i = 0; i < fields.length; i++) {
                    assert.commandWorked(db[myCollName].insert({_id: docIds[i], [fields[i]]: 1}));
                }
                for (let i = 0; i < fields.length; i++) {
                    assert.commandWorked(
                        db[myCollName].update({_id: docIds[i]}, {$set: {[fields[i]]: [1, 2]}}),
                    );
                }
            } catch (e) {
                if (tolerateCollDropError(this, e)) return;
                throw e;
            }

            const fullNs = db.getName() + "." + myCollName;
            verifyDirectMultikey(
                this,
                db,
                myCollName,
                {"ns": fullNs, "op": "u", "o2._id": docIds[0]},
                {"ns": fullNs, "op": "u", "o2._id": docIds[fields.length - 1]},
                IndexType.REGULAR,
                null,
                fields,
            );
        },

        dropAndRecreateColl: function dropAndRecreateColl(db, collName) {
            // Non-primary-driven index builds are known to leak multikey changes on the primary
            // side in the presence of aborted transactions. See SERVER-126285.
            if (!this.usesPrimaryDrivenIndexBuilds) return;

            const myCollName = pickPoolColl();
            db[myCollName].drop();

            // Tolerate IndexBuildAborted: another thread may drop the coll again while these
            // index builds are running.
            assert.commandWorkedOrFailedWithCode(
                db[myCollName].createIndex({"$**": 1}),
                ErrorCodes.IndexBuildAborted,
            );
            for (const f of REGULAR_INDEX_FIELDS) {
                assert.commandWorkedOrFailedWithCode(
                    db[myCollName].createIndex({[f]: 1}),
                    ErrorCodes.IndexBuildAborted,
                );
            }
        },

        dropAndRecreateIndex: function dropAndRecreateIndex(db, collName) {
            // Non-primary-driven index builds are known to leak multikey changes on the primary
            // side in the presence of aborted transactions. See SERVER-126285.
            if (!this.usesPrimaryDrivenIndexBuilds) return;

            const myCollName = pickPoolColl();
            const targets = [{"$**": 1}].concat(REGULAR_INDEX_FIELDS.map((f) => ({[f]: 1})));
            const target = targets[Random.randInt(targets.length)];

            // Tolerate races with other threads dropping or recreating the same index, and
            // with dropAndRecreateColl removing the entire collection:
            //   IndexNotFound: another thread already dropped this index.
            //   NamespaceNotFound: dropAndRecreateColl dropped the collection.
            //   IndexBuildAborted: a concurrent dropIndexes/dropCollection aborted our build.
            assert.commandWorkedOrFailedWithCode(db[myCollName].dropIndex(target), [
                ErrorCodes.IndexNotFound,
                ErrorCodes.NamespaceNotFound,
            ]);
            assert.commandWorkedOrFailedWithCode(
                db[myCollName].createIndex(target),
                ErrorCodes.IndexBuildAborted,
            );
        },

        // Batch pressure.
        insertNoise: function insertNoise(db, collName) {
            assert.commandWorked(
                db["padding_noise"].insert({noise: Random.randInt(100000), tid: this.tid}),
            );
        },
    };

    function setup(db, collName, cluster) {
        if (!cluster.isReplication()) return;

        for (let i = 0; i < POOL_SIZE; i++) {
            const coll = poolCollName(i);
            assert.commandWorked(db[coll].createIndex({"$**": 1}));
            for (const f of REGULAR_INDEX_FIELDS) {
                assert.commandWorked(db[coll].createIndex({[f]: 1}));
            }
        }
        this.secondaryHost = cluster.getSecondaryHost(db.getName());

        // Certain transitions are only enabled when using primary-driven index builds, which avoid
        // the IndexBuildInterceptor multikey-path leak (see SERVER-126285) that makes multikeyness
        // inconsistent between primary and secondaries.
        this.usesPrimaryDrivenIndexBuilds = PersistenceProviderUtil.allNodesHavePropertyWithValue(
            db,
            "mustUsePrimaryDrivenIndexBuilds",
            true,
        );

        cluster.awaitReplication();
    }

    function teardown(db, collName, cluster) {
        if (cluster.isReplication()) {
            cluster.awaitReplication();
        }
    }

    const allTransitions = {
        insertNoise: 0.3,
        createCollAndInsertWildcardMkInTxn: 0.05,
        createCollAndInsertRegularMkInTxn: 0.05,
        createCollAndUpdateWildcardMkInTxn: 0.05,
        createCollAndUpdateRegularMkInTxn: 0.05,
        insertWildcardMkInTxn: 0.05,
        insertRegularMkInTxn: 0.05,
        updateWildcardMkInTxn: 0.05,
        updateRegularMkInTxn: 0.05,
        insertWildcardMkDirect: 0.05,
        insertRegularMkDirect: 0.05,
        updateWildcardMkDirect: 0.05,
        updateRegularMkDirect: 0.05,
        // Gated at runtime in the state bodies on `mustUsePrimaryDrivenIndexBuilds`: no-op on
        // fixtures that don't use primary-driven index builds, see SERVER-126285.
        dropAndRecreateIndex: 0.05,
        dropAndRecreateColl: 0.05,
    };

    let transitions = {init: allTransitions};
    for (const state of Object.keys(allTransitions)) {
        transitions[state] = allTransitions;
    }

    return {
        threadCount: 10,
        iterations: 100,
        startState: "init",
        states: states,
        transitions: transitions,
        data: {retryOnKilledSession: false, prepareProbability: 0.5},
        setup: setup,
        teardown: teardown,
    };
})();
