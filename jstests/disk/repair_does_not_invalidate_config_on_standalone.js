/**
 * Tests that corruption on a standalone does not create a replica set configuration document.
 *
 * @tags: [requires_wiredtiger]
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');

const baseName = "repair_does_not_invalidate_config_on_standalone";
const dbName = baseName;
const collName = "test";

const dbpath = MongoRunner.dataPath + baseName + "/";
resetDbpath(dbpath);

let mongod = MongoRunner.runMongod({dbpath: dbpath});
const port = mongod.port;

let testColl = mongod.getDB(dbName)[collName];

assert.commandWorked(testColl.insert({_id: 0, foo: "bar"}));

let collUri = getUriForColl(testColl);
let collFile = dbpath + "/" + collUri + ".wt";

MongoRunner.stopMongod(mongod);

jsTestLog("Deleting collection file: " + collFile);
removeFile(collFile);

assertRepairSucceeds(dbpath, port);

assertStartAndStopStandaloneOnExistingDbpath(dbpath, port, function(node) {
    let nodeDB = node.getDB(dbName);
    assert(nodeDB[collName].exists());
    assert.eq(nodeDB[collName].find().itcount(), 0);

    assert(!nodeDB.getSiblingDB("local")["system.replset"].exists());
});
})();
