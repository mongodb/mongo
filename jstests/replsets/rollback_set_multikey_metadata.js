/**
 * Tests rollback of the setMultikeyMetadata oplog entry.
 *
 * Inside a multi-document transaction, a write that triggers a multikey transition is replicated
 * via a side transaction that emits an explicit setMultikeyMetadata op:'c' oplog entry. When that
 * side-transaction commit is rolled back, the in-memory multikey bit on the rollback node must be
 * reverted along with the durable _mdb_catalog row. This test verifies the rollback semantics for
 * both regular btree and wildcard indexes.
 *
 * @tags: [
 *   requires_mongobridge,
 *   requires_replication,
 *   uses_transactions,
 *   requires_fcv_90,
 *   featureFlagReplicateMultikeynessInTransactions,
 *   multiversion_incompatible,
 * ]
 */

import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const testName = jsTestName();
const dbName = testName;
const btreeCollName = "btreeColl";
const wildcardCollName = "wildcardColl";

function getCatalogIndex(db, collName, idxName) {
    const entries = db[collName].aggregate([{$listCatalog: {}}]).toArray();
    assert.eq(1, entries.length, "expected one catalog entry", {collName, entries});
    const idx = entries[0].md.indexes.find((i) => i.spec.name === idxName);
    assert(idx, "index not found in catalog", {collName, idxName, indexes: entries[0].md.indexes});
    return idx;
}

const CommonOps = (node) => {
    const testDb = node.getDB(dbName);
    const majority = {writeConcern: {w: "majority"}};

    assert.commandWorked(testDb.createCollection(btreeCollName, majority));
    assert.commandWorked(
        testDb.runCommand({
            createIndexes: btreeCollName,
            indexes: [{key: {"a.b": 1}, name: "a.b_1"}],
            writeConcern: {w: "majority"},
        }),
    );

    assert.commandWorked(testDb.createCollection(wildcardCollName, majority));
    assert.commandWorked(
        testDb.runCommand({
            createIndexes: wildcardCollName,
            indexes: [{key: {"$**": 1}, name: "$**_1"}],
            writeConcern: {w: "majority"},
        }),
    );

    const localDb = node.getDB("local");
    const preExisting = localDb.oplog.rs
        .find({op: "c", "o.setMultikeyMetadata": {$exists: true}})
        .toArray();
    assert.eq(
        0,
        preExisting.length,
        "no setMultikeyMetadata entries should exist pre-RollbackOps",
        {preExisting},
    );
};

const RollbackOps = (node) => {
    const session = node.startSession();
    const sessionDB = session.getDatabase(dbName);

    session.startTransaction();
    assert.commandWorked(sessionDB[btreeCollName].insert({a: [{b: 1}, {b: 2}]}));
    assert.commandWorked(session.commitTransaction_forTesting());

    session.startTransaction();
    assert.commandWorked(sessionDB[wildcardCollName].insert({a: [1, 2, 3]}));
    assert.commandWorked(session.commitTransaction_forTesting());

    session.endSession();

    const localDb = node.getDB("local");
    const preRollbackEntries = localDb.oplog.rs
        .find({op: "c", "o.setMultikeyMetadata": {$exists: true}})
        .toArray();
    assert.eq(
        2,
        preRollbackEntries.length,
        "expected one setMultikeyMetadata entry per index pre-rollback",
        {
            preRollbackEntries,
        },
    );

    const indexes = preRollbackEntries.map((e) => e.o.idxName).sort();
    assert.eq(["$**_1", "a.b_1"], indexes, "expected entries for both indexes", {indexes});

    const testDb = node.getDB(dbName);
    const btreeIdx = getCatalogIndex(testDb, btreeCollName, "a.b_1");
    assert(
        btreeIdx.multikey,
        "btree catalog must show multikey=true after side-txn commit pre-rollback",
        {btreeIdx},
    );
    const wildcardIdx = getCatalogIndex(testDb, wildcardCollName, "$**_1");
    assert(
        wildcardIdx.multikey,
        "wildcard catalog must show multikey=true after side-txn commit pre-rollback",
        {
            wildcardIdx,
        },
    );
};

const rollbackTest = new RollbackTest(testName);
CommonOps(rollbackTest.getPrimary());

const rollbackNode = rollbackTest.transitionToRollbackOperations();
RollbackOps(rollbackNode);

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

for (const node of rollbackTest.getTestFixture().nodes) {
    const testDb = node.getDB(dbName);

    assert.eq(
        0,
        testDb[btreeCollName].countDocuments({}),
        "btree collection has rolled-back docs",
        {host: node.host},
    );
    assert.eq(
        0,
        testDb[wildcardCollName].countDocuments({}),
        "wildcard collection has rolled-back docs",
        {
            host: node.host,
        },
    );

    const localDb = node.getDB("local");
    const orphans = localDb.oplog.rs
        .find({op: "c", "o.setMultikeyMetadata": {$exists: true}})
        .toArray();
    assert.eq(0, orphans.length, "orphan setMultikeyMetadata entries", {host: node.host, orphans});

    const btreeIdx = getCatalogIndex(testDb, btreeCollName, "a.b_1");
    assert(!btreeIdx.multikey, "btree catalog must show multikey=false post-rollback", {
        host: node.host,
        btreeIdx,
    });
    const wildcardIdx = getCatalogIndex(testDb, wildcardCollName, "$**_1");
    assert(!wildcardIdx.multikey, "wildcard catalog must show multikey=false post-rollback", {
        host: node.host,
        wildcardIdx,
    });
}

rollbackTest.stop();
