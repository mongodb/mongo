/**
 * Test that index keys with empty string values are allowed on 4.0 and that upgrading with
 * such indexes will succeed.
 */

(function() {
'use strict';

load('jstests/libs/get_index_helpers.js');

const dbpath = MongoRunner.dataPath + 'empty_string_index_value';
resetDbpath(dbpath);

const oldVersion = '4.0';
const newVersion = 'latest';

// We set noCleanData to true in order to preserve the data files across mongod restart.
const mongodOptions = {
    dbpath: dbpath,
    noCleanData: true,
    binVersion: oldVersion
};

// Start up an old binary version mongod.
let conn = MongoRunner.runMongod(mongodOptions);

assert.neq(null, conn, `mongod was unable able to start with version ${oldVersion}`);

// Set up a collection on a 4.0 binary version node with one document and an index with
// an empty string as index value, and then shut it down.
let testDB = conn.getDB('test');
assert.commandWorked(testDB.createCollection('testColl'));
assert.commandWorked(testDB.testColl.insert({a: 1}));
assert.commandWorked(testDB.testColl.createIndex({a: ""}));
MongoRunner.stopMongod(conn);

// Restart the mongod with the latest binary version and the 4.0 version data files.
mongodOptions.binVersion = newVersion;
conn = MongoRunner.runMongod(mongodOptions);
assert.neq(null, conn);

// Confirm that mongod startup does not fail due to the index specification
// containing an empty string.
testDB = conn.getDB('test');
testDB.testColl.find();
assert.eq(1,
          testDB.testColl.count({}, {hint: {a: ""}}),
          `data from ${oldVersion} should be available; options: ` + tojson(mongodOptions));

assert.neq(null,
           GetIndexHelpers.findByKeyPattern(testDB.testColl.getIndexes(), {a: ""}),
           `index from ${oldVersion} should be available; options: ` + tojson(mongodOptions));

// Verify that indexes with empty string values cannot be created
assert.commandFailedWithCode(testDB.testColl.createIndex({x: ""}), ErrorCodes.CannotCreateIndex);

MongoRunner.stopMongod(conn);
})();
