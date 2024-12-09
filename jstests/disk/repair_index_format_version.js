/**
 * Tests that mismatch of index type and index format version will be resolved during startup.
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');

const baseName = "repair_index_format_version";
const collNamePrefix = "test_";
let count = 0;
const dbpath = MongoRunner.dataPath + baseName + "/";

resetDbpath(dbpath);

jsTestLog("Repair the format version of a unique index.");

// Uses the modified data files in the same dbpath over restarts.
let mongod = startMongodOnExistingPath(dbpath);
let db = mongod.getDB(baseName);
let collName = collNamePrefix + count++;
db.createCollection(collName);
let testColl = db[collName];
assert.commandWorked(testColl.createIndex({a: 1}, {unique: true}));

let uri = getUriForIndex(testColl, "a_1");
alterIndexFormatVersion(uri, mongod, 8);

mongod = startMongodOnExistingPath(dbpath);
checkLog.containsJson(mongod, 6818600);

jsTestLog("Repair the format version of a non-unique index.");

db = mongod.getDB(baseName);
collName = collNamePrefix + count++;
db.createCollection(collName);
testColl = db[collName];
assert.commandWorked(testColl.createIndex({b: 1}));

uri = getUriForIndex(testColl, "b_1");
alterIndexFormatVersion(uri, mongod, 14);

mongod = startMongodOnExistingPath(dbpath);
checkLog.containsJson(mongod, 6818600);

MongoRunner.stopMongod(mongod, null, {skipValidation: true});
})();
