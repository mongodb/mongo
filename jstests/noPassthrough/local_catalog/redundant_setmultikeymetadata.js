/**
 * A setMultikeyMetadata oplog entry is emitted only when a write makes a new path multikey;
 * re-inserting an array at an already-multikey path is a no-op and must not emit another entry.
 * This holds for both wildcard indexes (multikey paths tracked as metadata keys inside the index)
 * and regular indexes (multikey paths tracked in the catalog). The entry is produced by the
 * multi-document-transaction side transaction, so each insert runs in its own txn.
 *
 * @tags: [
 *   featureFlagReplicateMultikeynessInTransactions,
 *   requires_replication,
 *   uses_transactions,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "redundant_setmultikeymetadata";

describe("setMultikeyMetadata is emitted only for newly multikey paths", function () {
    let rst;
    let primary;
    let primaryDB;
    let cmdNs;

    before(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        primaryDB = primary.getDB(dbName);
        cmdNs = primaryDB.getCollection("$cmd").getFullName();
    });

    after(function () {
        if (rst) {
            rst.stopSet();
            rst = null;
        }
    });

    function countSetMultikeyMetadata(coll, indexName) {
        return rst.dumpOplog(primary, {
            op: "c",
            ns: cmdNs,
            "o.setMultikeyMetadata": coll.getFullName(),
            "o.idxName": indexName,
        }).length;
    }

    function insertAllInTxn(collName, docs) {
        const session = primary.startSession();
        try {
            const sessionColl = session.getDatabase(dbName).getCollection(collName);
            session.startTransaction();
            for (const doc of docs) {
                assert.commandWorked(sessionColl.insert(doc));
            }
            assert.commandWorked(session.commitTransaction_forTesting());
        } finally {
            session.endSession();
        }
    }

    function runScenario({
        label,
        collName,
        indexName,
        indexSpec,
        firstDoc,
        samePathDoc,
        newPathDoc,
    }) {
        const coll = primaryDB.getCollection(collName);
        assert.commandWorked(primaryDB.runCommand({drop: collName}));
        assert.commandWorked(coll.createIndex(indexSpec, {name: indexName}));

        // Insert a doc with a new multikey path, expect a single oplog entry.
        insertAllInTxn(collName, [firstDoc]);
        assert.eq(
            1,
            countSetMultikeyMetadata(coll, indexName),
            `${label}: expected one setMultikeyMetadata entry after the first array insert`,
        );

        // Insert a doc with the same multikey path, expect no new oplog entries (total 1).
        insertAllInTxn(collName, [samePathDoc]);
        assert.eq(
            1,
            countSetMultikeyMetadata(coll, indexName),
            `${label}: must not re-emit setMultikeyMetadata for an already-multikey path`,
        );

        // Insert a doc with another new multikey path, expect one more oplog entry (total 2).
        insertAllInTxn(collName, [newPathDoc]);
        assert.eq(
            2,
            countSetMultikeyMetadata(coll, indexName),
            `${label}: must emit setMultikeyMetadata again when a new path becomes multikey`,
        );
    }

    function runWithinTxnScenario({label, collName, indexName, indexSpec, samePathDocs}) {
        const coll = primaryDB.getCollection(collName);
        assert.commandWorked(primaryDB.runCommand({drop: collName}));
        assert.commandWorked(coll.createIndex(indexSpec, {name: indexName}));

        insertAllInTxn(collName, samePathDocs);
        assert.eq(
            1,
            countSetMultikeyMetadata(coll, indexName),
            `${label}: making the same path multikey twice in one txn must emit setMultikeyMetadata once`,
        );
    }

    function runMixedPathScenario({label, collName, indexName, indexSpec, firstDoc, mixedDoc}) {
        const coll = primaryDB.getCollection(collName);
        assert.commandWorked(primaryDB.runCommand({drop: collName}));
        assert.commandWorked(coll.createIndex(indexSpec, {name: indexName}));

        insertAllInTxn(collName, [firstDoc]);
        assert.eq(
            1,
            countSetMultikeyMetadata(coll, indexName),
            `${label}: one setMultikeyMetadata entry after the first array insert`,
        );

        // Only the new path is newly multikey here, so the delta must be exactly one.
        insertAllInTxn(collName, [mixedDoc]);
        assert.eq(
            2,
            countSetMultikeyMetadata(coll, indexName),
            `${label}: one insert touching an already-multikey path and a new path must emit setMultikeyMetadata once, for the new path only`,
        );
    }

    it("wildcard index", function () {
        runScenario({
            label: "wildcard",
            collName: "wildcard",
            indexName: "wildcard_1",
            indexSpec: {"$**": 1},
            firstDoc: {_id: 1, a: [1, 2]},
            samePathDoc: {_id: 2, a: [3, 4]},
            newPathDoc: {_id: 3, c: [5, 6]},
        });
    });

    it("compound regular index", function () {
        runScenario({
            label: "compound regular",
            collName: "regular",
            indexName: "a_1_b_1",
            indexSpec: {a: 1, b: 1},
            firstDoc: {_id: 1, a: [1, 2]},
            samePathDoc: {_id: 2, a: [3, 4]},
            newPathDoc: {_id: 3, b: [5, 6]},
        });
    });

    it("wildcard index, same path twice in one transaction", function () {
        runWithinTxnScenario({
            label: "wildcard",
            collName: "wildcard_within_txn",
            indexName: "wildcard_1",
            indexSpec: {"$**": 1},
            samePathDocs: [
                {_id: 1, a: [1, 2]},
                {_id: 2, a: [3, 4]},
            ],
        });
    });

    it("compound regular index, same path twice in one transaction", function () {
        runWithinTxnScenario({
            label: "compound regular",
            collName: "regular_within_txn",
            indexName: "a_1_b_1",
            indexSpec: {a: 1, b: 1},
            samePathDocs: [
                {_id: 1, a: [1, 2]},
                {_id: 2, a: [3, 4]},
            ],
        });
    });

    // No compound-regular counterpart: a compound index rejects a single document with two array
    // paths (CannotIndexParallelArrays), so one insert can never make two of its paths multikey.
    // Only multi-path indexes such as wildcard can, which is what this case covers.
    it("wildcard index, one insert making an existing path and a new path multikey", function () {
        runMixedPathScenario({
            label: "wildcard",
            collName: "wildcard_mixed",
            indexName: "wildcard_1",
            indexSpec: {"$**": 1},
            firstDoc: {_id: 1, a: [1, 2]},
            mixedDoc: {_id: 2, a: [3, 4], c: [5, 6]},
        });
    });
});
