/*
 * Test of a successfull replica set rollback for basic CRUD operations in multitenancy environment
 * with featureFlagRequireTenantId. This test is modeled from rollback_crud_ops_sequence.js.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const kColl = "bar";
const tenantA = ObjectId();
const tenantB = ObjectId();
const tokenTenantA = _createTenantToken({tenant: tenantA});
const tokenTenantB = _createTenantToken({tenant: tenantB});

const insertDocs = function(db, coll, token, documents) {
    db.getMongo()._setSecurityToken(token);
    assert.commandWorked(db.runCommand({insert: coll, documents}));
};

const updateDocs = function(db, coll, token, updates) {
    db.getMongo()._setSecurityToken(token);
    assert.commandWorked(db.runCommand({update: coll, updates}));
};

const deleteMany = function(db, coll, token, query) {
    db.getMongo()._setSecurityToken(token);
    assert.commandWorked(db.runCommand({
        delete: coll,
        deletes: [
            {q: query, limit: 0},
        ]
    }));
};

const validateCounts = function(db, coll, token, expect) {
    db.getMongo()._setSecurityToken(token);
    for (let expected of expect) {
        let res = db.runCommand({count: coll, query: expected.q});
        assert.eq(res.n, expected.n);
    }
};

// Helper function for verifying contents at the end of the test.
const checkFinalResults = function(db) {
    validateCounts(db, kColl, tokenTenantA, [
        {q: {q: 70}, n: 0},
        {q: {q: 40}, n: 2},
        {q: {a: 'foo'}, n: 3},
        {q: {q: {$gt: -1}}, n: 6},
        {q: {txt: 'foo'}, n: 1},
        {q: {q: 4}, n: 0}
    ]);

    validateCounts(db, kColl, tokenTenantB, [{q: {q: 1}, n: 1}, {q: {q: 40}, n: 0}]);

    db.getMongo()._setSecurityToken(tokenTenantA);
    let res = db.runCommand({find: kColl, filter: {q: 0}});
    assert.eq(res.cursor.firstBatch.length, 1);
    assert.eq(res.cursor.firstBatch[0].y, 33);

    res = db.runCommand({find: 'kap'});
    assert.eq(res.cursor.firstBatch.length, 1);

    res = db.runCommand({find: 'kap2'});
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
        nodeOptions: {
            setParameter: {
                multitenancySupport: true,
                featureFlagRequireTenantID: true,
                featureFlagSecurityToken: true
            }
        }
    });
    replSet.startSet();
    replSet.nodes.forEach(setFastGetMoreEnabled);

    let config = replSet.getReplSetConfig();
    config.members[2].priority = 0;
    config.settings = {chainingAllowed: false};
    replSet.initiate(config);
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
insertDocs(rollbackNodeDB, kColl, tokenTenantA, [{q: -2}, {q: 0}, {q: 1, a: "foo"}]);
insertDocs(rollbackNodeDB, kColl, tokenTenantB, [{q: 1}, {q: 40, a: "foo"}]);
insertDocs(rollbackNodeDB, kColl, tokenTenantA, [
    {q: 2, a: "foo", x: 1},
    {q: 3, bb: 9, a: "foo"},
    {q: 40, a: 1},
    {q: 40, a: 2},
    {q: 70, txt: 'willremove'}
]);

// Testing capped collection.
rollbackNodeDB.createCollection("kap", {capped: true, size: 5000});
insertDocs(rollbackNodeDB, 'kap', tokenTenantA, [{foo: 1}]);
// Going back to empty on capped is a special case and must be tested.
rollbackNodeDB.createCollection("kap2", {capped: true, size: 5000});

rollbackNode._setSecurityToken(undefined);
syncSource._setSecurityToken(undefined);

rollbackTest.awaitReplication();
rollbackTest.transitionToRollbackOperations();

// These operations are only done on 'rollbackNode' and should eventually be rolled back.
insertDocs(rollbackNodeDB, kColl, tokenTenantA, [{q: 4}]);
updateDocs(rollbackNodeDB, kColl, tokenTenantA, [
    {q: {q: 3}, u: {q: 3, rb: true}},
]);
insertDocs(rollbackNodeDB, kColl, tokenTenantB, [{q: 1, foo: 2}]);
deleteMany(rollbackNodeDB, kColl, tokenTenantA, {q: 40});
updateDocs(rollbackNodeDB, kColl, tokenTenantA, [
    {q: {q: 2}, u: {q: 39, rb: true}},
]);

// Rolling back a delete will involve reinserting the item(s).
deleteMany(rollbackNodeDB, kColl, tokenTenantA, {q: 1});
updateDocs(rollbackNodeDB, kColl, tokenTenantA, [
    {q: {q: 0}, u: {$inc: {y: 1}}},
]);
insertDocs(rollbackNodeDB, 'kap', tokenTenantA, [{foo: 2}]);
insertDocs(rollbackNodeDB, 'kap2', tokenTenantA, [{foo: 2}]);

// Create a collection (need to roll back the whole thing).
insertDocs(rollbackNodeDB, 'newcoll', tokenTenantA, [{a: true}]);
// Create a new empty collection (need to roll back the whole thing).
assert.commandWorked(rollbackNodeDB.createCollection("abc"));

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

// Insert new data into syncSource so that rollbackNode enters rollback when it is reconnected.
// These operations should not be rolled back.
insertDocs(syncSourceDB, kColl, tokenTenantA, [{txt: 'foo'}]);
deleteMany(syncSourceDB, kColl, tokenTenantA, {q: 70});
updateDocs(syncSourceDB, kColl, tokenTenantA, [
    {q: {q: 0}, u: {$inc: {y: 33}}},
]);
deleteMany(syncSourceDB, kColl, tokenTenantB, {q: 40});

rollbackNode._setSecurityToken(undefined);
syncSource._setSecurityToken(undefined);

rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

rollbackTest.awaitReplication();
checkFinalResults(rollbackNodeDB);
checkFinalResults(syncSourceDB);

rollbackNode._setSecurityToken(undefined);
syncSource._setSecurityToken(undefined);

rollbackTest.stop();
