/**
 * This test ensures that a failure during repair does not allow MongoDB to start normally and
 * requires it to be restarted with --repair again.
 *
 * This is not storage-engine specific.
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');

const exitBeforeRepairParameter = "exitBeforeDataRepair";
const exitBeforeRepairInvalidatesConfigParameter = "exitBeforeRepairInvalidatesConfig";

const baseName = "repair_failure_is_recoverable";
const dbName = "repair_failure_is_recoverable";
const collName = "test";

const dbpath = MongoRunner.dataPath + baseName + "/";
resetDbpath(dbpath);

let mongod = MongoRunner.runMongod({dbpath: dbpath});
const port = mongod.port;

let testColl = mongod.getDB(dbName)[collName];

assert.commandWorked(testColl.insert({_id: 0, foo: "bar"}));

MongoRunner.stopMongod(mongod);

/**
 * Test 1. Cause an exit before repairing data. MongoDB should not be able to restart without
 * --repair.
 */
assertRepairFailsWithFailpoint(dbpath, port, exitBeforeRepairParameter);

assertErrorOnStartupAfterIncompleteRepair(dbpath, port);

assertRepairSucceeds(dbpath, port);

assertStartAndStopStandaloneOnExistingDbpath(dbpath, port, function(node) {
    let nodeDB = node.getDB(dbName);
    assert(nodeDB[collName].exists());
    assert.eq(nodeDB[collName].find().itcount(), 1);
});

/**
 * Test 2. Fail after repairing data, before invalidating the replica set config. MongoDB should
 * not be able to restart without --repair.
 */
assertRepairFailsWithFailpoint(dbpath, port, exitBeforeRepairInvalidatesConfigParameter);

assertErrorOnStartupAfterIncompleteRepair(dbpath, port);

assertRepairSucceeds(dbpath, port);

assertStartAndStopStandaloneOnExistingDbpath(dbpath, port, function(node) {
    let nodeDB = node.getDB(dbName);
    assert(nodeDB[collName].exists());
    assert.eq(nodeDB[collName].find().itcount(), 1);
});
})();
