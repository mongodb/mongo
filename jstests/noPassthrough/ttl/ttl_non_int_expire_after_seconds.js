/**
 * Tests TTL indexes with non-int values for 'expireAfterSeconds'.
 *
 * Starting v7.3+, the 'expireAfterSeconds' index field is automatically truncated from a floating
 * point to its integer equivalent before getting stored in the catalog. This is because users and
 * tools rely on $listIndexes to rebuild a collection's indexes and $listIndexes reports
 * 'expireAfterSeconds' as a 32 bit integer as part of the stable API.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const nonIntVal = 10000.7;
const intVal = Math.floor(nonIntVal);

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {votes: 0, priority: 0}}],
    nodeOptions: {setParameter: {ttlMonitorSleepSecs: 5}},
    // Sync from primary only so that we have a well-defined node to check listIndexes behavior.
    settings: {chainingAllowed: false},
});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

let primary = rst.getPrimary();
const dbName = 'test';
const collName = 'coll';
const db = primary.getDB(dbName);
const coll = db[collName];

function assertExpireAfterSeconds(coll, indexName, expectedExpireAfterSeconds) {
    const indexes = coll.aggregate({$listCatalog: {}}).toArray()[0].md.indexes;
    const index = indexes.find(index => index.spec.name === indexName);
    assert(index,
           `Expected to find index with ${indexName} in the catalog. Found catalog indexes: ${
               tojson(index)}`);
    const actualExpireAfterSeconds = index.spec.expireAfterSeconds;
    assert.eq(expectedExpireAfterSeconds,
              actualExpireAfterSeconds,
              `Unexpected expireAfterSeconds field for index: ${tojson(index)}`);
}

function assertExpireAfterSecondsAcrossNodes(indexName, expectedExpireAfterSeconds) {
    rst.awaitReplication();
    rst.nodes.forEach(node => {
        assert.soonNoExcept(
            () => {
                const nodeColl = node.getDB(dbName)[collName];
                assertExpireAfterSeconds(nodeColl, indexName, expectedExpireAfterSeconds);
                return true;
            },
            `Node ${node.host} has unexpected index catalog results. ${
                tojson(node.getDB(dbName)[collName].aggregate([{$indexStats: {}}]).toArray())}`);
    });
}

// Current 'createIndex' behavior automatically truncates a non-int 'expireAfterSeconds' to
// its integer value during validation. Thus, we use a failpoint to disable current behavior and
// simulate a non-int value leftover from older MongoDB versions.
function createIndexWithoutExpireAfterSecondsValidation(coll, indexName, expireAfterSeconds) {
    const fpCreate = configureFailPoint(primary, 'skipTTLIndexExpireAfterSecondsValidation');
    try {
        assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: nonIntVal}));
    } finally {
        fpCreate.off();
    }
}

// Tests that by default, a non-int 'expireAfterSeconds' value is automatically truncated to an
// integer value.
(function testNonIntToIntByDefault() {
    assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: nonIntVal}));
    assertExpireAfterSecondsAcrossNodes('t_1', intVal);
    assert.commandWorked(coll.dropIndex('t_1'));
}());

// Tests that a non-int 'expireAfterSeconds' value stored in the catalog can be converted to its
// truncated integer equivalent via collMod.
(function testNonIntToIntByCollMod() {
    // Force the catalog to store a non-int 'expireAfterSeconds' to simulate behavior from older
    // versions of the server.
    createIndexWithoutExpireAfterSecondsValidation(coll, 't_1', nonIntVal);
    // Ensure the non-int value is stored in the catalog for both the primary and the secondaries.
    assertExpireAfterSecondsAcrossNodes('t_1', nonIntVal);

    assert.commandWorked(db.runCommand(
        {collMod: collName, index: {keyPattern: {t: 1}, expireAfterSeconds: intVal}}));
    assertExpireAfterSecondsAcrossNodes('t_1', intVal);

    assert.commandWorked(coll.dropIndex('t_1'));
}());

// Tests that a node that undergoes initial sync will sync the int version of a non-int
// 'expireAfterSeconds' from the primary.
//
// Note: This contrasts the standard replication behavior where the non-int value is replicated
// to secondaries.
(function testNonIntWithInitialSyncResultsInInt() {
    createIndexWithoutExpireAfterSecondsValidation(coll, 't_1', nonIntVal);
    assertExpireAfterSeconds(coll, 't_1', nonIntVal);

    const newNode = rst.add({rsConfig: {votes: 0, priority: 0}});
    rst.reInitiate();
    rst.awaitSecondaryNodes(null, [newNode]);
    rst.awaitReplication();
    let newNodeTestDB = newNode.getDB(dbName);
    let newNodeColl = newNodeTestDB.getCollection(collName);
    assertExpireAfterSeconds(newNodeColl, 't_1', intVal);
    assert.commandWorked(coll.dropIndex('t_1'));
}());

rst.stopSet();
