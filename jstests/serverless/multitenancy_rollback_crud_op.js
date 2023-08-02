/*
 * Test of a successfull replica set rollback for basic CRUD operations in multitenancy environment
 * with featureFlagRequireTenantId. This test is modeled from rollback_crud_ops_sequence.js.
 */
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const kColl = "bar";
const tenantA = ObjectId();
const tenantB = ObjectId();

const insertDocs = function(db, coll, tenant, documents) {
    assert.commandWorked(db.runCommand({insert: coll, documents, '$tenant': tenant}));
};

const updateDocs = function(db, coll, tenant, updates) {
    assert.commandWorked(db.runCommand({update: coll, updates, '$tenant': tenant}));
};

const deleteMany = function(db, coll, tenant, query) {
    assert.commandWorked(db.runCommand({
        delete: coll,
        deletes: [
            {q: query, limit: 0},
        ],
        '$tenant': tenant

    }));
};

const validateCounts = function(db, coll, tenant, expect) {
    for (let expected of expect) {
        let res = db.runCommand({count: coll, query: expected.q, '$tenant': tenant});
        assert.eq(res.n, expected.n);
    }
};

// Helper function for verifying contents at the end of the test.
const checkFinalResults = function(db) {
    validateCounts(db, kColl, tenantA, [
        {q: {q: 70}, n: 0},
        {q: {q: 40}, n: 2},
        {q: {a: 'foo'}, n: 3},
        {q: {q: {$gt: -1}}, n: 6},
        {q: {txt: 'foo'}, n: 1},
        {q: {q: 4}, n: 0}
    ]);

    validateCounts(db, kColl, tenantB, [{q: {q: 1}, n: 1}, {q: {q: 40}, n: 0}]);

    let res = db.runCommand({find: kColl, filter: {q: 0}, '$tenant': tenantA});
    assert.eq(res.cursor.firstBatch.length, 1);
    assert.eq(res.cursor.firstBatch[0].y, 33);

    res = db.runCommand({find: 'kap', '$tenant': tenantA});
    assert.eq(res.cursor.firstBatch.length, 1);

    res = db.runCommand({find: 'kap2', '$tenant': tenantA});
    assert.eq(res.cursor.firstBatch.length, 0);
};

function setFastGetMoreEnabled(node) {
    assert.commandWorked(
        node.adminCommand({configureFailPoint: 'setSmallOplogGetMoreMaxTimeMS', mode: 'alwaysOn'}),
        `Failed to enable setSmallOplogGetMoreMaxTimeMS failpoint.`);
}

function setUpRst() {
    const replSet = new ReplSetTest({
        nodes: 3,
        useBridge: true,
        nodeOptions: {setParameter: {multitenancySupport: true, featureFlagRequireTenantID: true}}
    });
    replSet.startSet();
    replSet.nodes.forEach(setFastGetMoreEnabled);

    let config = replSet.getReplSetConfig();
    config.members[2].priority = 0;
    config.settings = {chainingAllowed: false};
    replSet.initiateWithHighElectionTimeout(config);
    // Tiebreaker's replication is paused for most of the test, avoid falling off the oplog.
    replSet.nodes.forEach((node) => {
        assert.commandWorked(node.adminCommand({replSetResizeOplog: 1, minRetentionHours: 2}));
    });

    assert.eq(replSet.nodes.length,
              3,
              "Mismatch between number of data bearing nodes and test configuration.");

    return replSet;
}

const replSet = setUpRst();
const rollbackTest = new RollbackTest("MultitenancyRollbackTest", replSet);

const rollbackNode = rollbackTest.getPrimary();
rollbackNode.setSecondaryOk();
const syncSource = rollbackTest.getSecondary();
syncSource.setSecondaryOk();

const rollbackNodeDB = rollbackNode.getDB("foo");
const syncSourceDB = syncSource.getDB("foo");

// Insert initial data for both nodes.
insertDocs(rollbackNodeDB, kColl, tenantA, [{q: -2}, {q: 0}, {q: 1, a: "foo"}]);
insertDocs(rollbackNodeDB, kColl, tenantB, [{q: 1}, {q: 40, a: "foo"}]);
insertDocs(rollbackNodeDB, kColl, tenantA, [
    {q: 2, a: "foo", x: 1},
    {q: 3, bb: 9, a: "foo"},
    {q: 40, a: 1},
    {q: 40, a: 2},
    {q: 70, txt: 'willremove'}
]);

// Testing capped collection.
rollbackNodeDB.createCollection("kap", {'$tenant': tenantA, capped: true, size: 5000});
insertDocs(rollbackNodeDB, 'kap', tenantA, [{foo: 1}]);
// Going back to empty on capped is a special case and must be tested.
rollbackNodeDB.createCollection("kap2", {'$tenant': tenantA, capped: true, size: 5000});

rollbackTest.awaitReplication();
rollbackTest.transitionToRollbackOperations();

// These operations are only done on 'rollbackNode' and should eventually be rolled back.
insertDocs(rollbackNodeDB, kColl, tenantA, [{q: 4}]);
updateDocs(rollbackNodeDB, kColl, tenantA, [
    {q: {q: 3}, u: {q: 3, rb: true}},
]);
insertDocs(rollbackNodeDB, kColl, tenantB, [{q: 1, foo: 2}]);
deleteMany(rollbackNodeDB, kColl, tenantA, {q: 40});
updateDocs(rollbackNodeDB, kColl, tenantA, [
    {q: {q: 2}, u: {q: 39, rb: true}},
]);

// Rolling back a delete will involve reinserting the item(s).
deleteMany(rollbackNodeDB, kColl, tenantA, {q: 1});
updateDocs(rollbackNodeDB, kColl, tenantA, [
    {q: {q: 0}, u: {$inc: {y: 1}}},
]);
insertDocs(rollbackNodeDB, 'kap', tenantA, [{foo: 2}]);
insertDocs(rollbackNodeDB, 'kap2', tenantA, [{foo: 2}]);

// Create a collection (need to roll back the whole thing).
insertDocs(rollbackNodeDB, 'newcoll', tenantA, [{a: true}]);
// Create a new empty collection (need to roll back the whole thing).
assert.commandWorked(rollbackNodeDB.createCollection("abc", {'$tenant': tenantA}));

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

// Insert new data into syncSource so that rollbackNode enters rollback when it is reconnected.
// These operations should not be rolled back.
insertDocs(syncSourceDB, kColl, tenantA, [{txt: 'foo'}]);
deleteMany(syncSourceDB, kColl, tenantA, {q: 70});
updateDocs(syncSourceDB, kColl, tenantA, [
    {q: {q: 0}, u: {$inc: {y: 33}}},
]);
deleteMany(syncSourceDB, kColl, tenantB, {q: 40});

rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

rollbackTest.awaitReplication();
checkFinalResults(rollbackNodeDB);
checkFinalResults(syncSourceDB);

rollbackTest.stop();
