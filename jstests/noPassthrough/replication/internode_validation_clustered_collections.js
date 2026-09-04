/**
 * Verifies that clustered collections take part in continuous internode validation end to end:
 * their oplog entries carry a per-document hash, and a secondary recomputes it as it applies them.
 *
 * Clustered collections have no replicated record ids, but their record ids follow from their
 * documents, which is what lets a secondary locate what it wrote. Time-series collections are
 * clustered on their buckets' OID _id, so they are covered here too.
 *
 * @tags: [
 *   requires_replication,
 *   requires_timeseries,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("continuous internode validation of clustered collections", function () {
    const kCrudOpTypes = ["i", "u", "d"];
    const kDbName = "internode_validation_clustered";
    const kTimeField = "time";

    /**
     * Returns every replicated CRUD operation on 'ns', flattening the inner operations of any
     * applyOps entry. Multi-document writes to a clustered collection are logged as a batched write
     * and transactions as an applyOps entry, so in both cases the operations exist only inside that
     * array.
     */
    function crudOperationsFor(ns) {
        const ops = [];
        for (const entry of this.primary.getDB("local").oplog.rs.find().toArray()) {
            if (entry.op === "c" && entry.o && Array.isArray(entry.o.applyOps)) {
                for (const innerOp of entry.o.applyOps) {
                    if (innerOp.ns === ns && kCrudOpTypes.includes(innerOp.op)) {
                        ops.push(innerOp);
                    }
                }
            } else if (entry.ns === ns && kCrudOpTypes.includes(entry.op)) {
                ops.push(entry);
            }
        }
        return ops;
    }

    /**
     * Asserts that every replicated CRUD operation on 'ns' carries a per-document hash, and that
     * there was at least one of each op type named in 'expectedOpTypes'. A single operation without
     * a hash removes the collection from the rolling collection hash permanently, so this asserts
     * over all of them rather than a sample.
     */
    function assertEveryOperationCarriesAHash(ns, expectedOpTypes) {
        const ops = crudOperationsFor.call(this, ns);
        assert.gt(ops.length, 0, "no CRUD oplog entries", {ns});

        for (const op of ops) {
            assert(
                op.m !== undefined && op.m.h !== undefined,
                "oplog entry carries no per-document hash",
                {ns, op},
            );
        }

        const seen = new Set(ops.map((op) => op.op));
        for (const opType of expectedOpTypes) {
            assert(seen.has(opType), "missing an op type", {ns, opType, seen: Array.from(seen)});
        }
    }

    /**
     * Returns the secondary's per-op-type hash mismatch counters.
     */
    function mismatchCounters() {
        return this.secondary.getDB("admin").serverStatus().metrics.repl.internodeConsistency
            .hashMismatch;
    }

    before(function () {
        this.rst = new ReplSetTest({
            nodes: 2,
            nodeOptions: {
                setParameter: {
                    featureFlagContinuousInternodeValidationPerDocument: true,
                    // The two are coupled in production: without it every entry carries a hash
                    // with no size delta, which the accumulator warns about.
                    featureFlagReplicatedFastCount: true,
                    // Updates and deletes on a clustered collection reach the hash check only
                    // through the by-record-id apply fast path, which a persistence provider can
                    // mandate on its own; here it has to be asked for.
                    featureFlagClusteredCollectionOplogApplyFastPath: true,
                },
            },
        });
        this.rst.startSet();
        this.rst.initiate();

        this.primary = this.rst.getPrimary();
        this.secondary = this.rst.getSecondary();
        const db = this.primary.getDB(kDbName);

        const initialCounters = mismatchCounters.call(this);
        assert.eq(
            initialCounters.insert + initialCounters.update + initialCounters.delete,
            0,
            "counters did not start at zero",
            {initialCounters},
        );

        // A user clustered collection, covering insert, update and delete.
        assert.commandWorked(
            db.createCollection("clustered", {clusteredIndex: {key: {_id: 1}, unique: true}}),
        );
        assert.commandWorked(
            db.clustered.insertMany([
                {_id: 1, a: 1},
                {_id: 2, a: 2},
                {_id: 3, a: 3},
            ]),
        );
        assert.commandWorked(db.clustered.update({_id: 1}, {$set: {a: 100}}));
        assert.commandWorked(db.clustered.remove({_id: 3}));

        // A string _id takes a different record id derivation than the integer and OID cases.
        assert.commandWorked(
            db.createCollection("stringId", {clusteredIndex: {key: {_id: 1}, unique: true}}),
        );
        assert.commandWorked(db.stringId.insert({_id: "some-string-id", a: 1}));
        assert.commandWorked(db.stringId.update({_id: "some-string-id"}, {$set: {a: 2}}));
        assert.commandWorked(db.stringId.remove({_id: "some-string-id"}));

        // The same clustered collection written inside a multi-document transaction, whose inner
        // ops carry their own hashes.
        const session = this.primary.startSession();
        try {
            const sessionColl = session.getDatabase(kDbName).clustered;
            session.startTransaction();
            assert.commandWorked(sessionColl.insert({_id: 10, a: 10}));
            assert.commandWorked(sessionColl.update({_id: 2}, {$set: {a: 200}}));
            assert.commandWorked(sessionColl.remove({_id: 10}));
            assert.commandWorked(session.commitTransaction_forTesting());
        } finally {
            session.endSession();
        }

        // A time-series collection, whose buckets are clustered on an OID _id. Both measurements
        // carry the same timestamp so they cannot fall into different buckets, which makes the
        // second insert deterministically an update of the first one's bucket.
        assert.commandWorked(db.createCollection("ts", {timeseries: {timeField: kTimeField}}));
        const measurementTime = new Date();
        assert.commandWorked(db.ts.insert({[kTimeField]: measurementTime, x: 1}));
        assert.commandWorked(db.ts.insert({[kTimeField]: measurementTime, x: 2}));
        assert.commandWorked(db.ts.deleteMany({x: {$gte: 0}}));

        this.rst.awaitReplication();
    });

    after(function () {
        this.rst.stopSet();
    });

    it("stamps a hash on every clustered collection write", function () {
        assertEveryOperationCarriesAHash.call(this, `${kDbName}.clustered`, ["i", "u", "d"]);
    });

    it("stamps a hash on a clustered collection keyed by a string _id", function () {
        assertEveryOperationCarriesAHash.call(this, `${kDbName}.stringId`, ["i", "u", "d"]);
    });

    it("stamps a hash on time-series bucket writes", function () {
        assertEveryOperationCarriesAHash.call(this, `${kDbName}.ts`, ["i", "u", "d"]);
    });

    it("verifies every write on the secondary without diverging", function () {
        assert.commandWorked(this.secondary.adminCommand({ping: 1}));

        const counters = mismatchCounters.call(this);
        assert.eq(
            counters.insert + counters.update + counters.delete,
            0,
            "the secondary reported a hash divergence",
            {counters},
        );

        for (const collName of ["clustered", "stringId", "ts"]) {
            const primaryDocs = this.primary
                .getDB(kDbName)
                [collName].find()
                .sort({_id: 1})
                .toArray();
            const secondaryDocs = this.secondary
                .getDB(kDbName)
                [collName].find()
                .sort({_id: 1})
                .readPref("secondary")
                .toArray();
            assert.eq(primaryDocs, secondaryDocs, "primary and secondary disagree", {
                collName,
                primaryDocs,
                secondaryDocs,
            });
        }
    });
});
