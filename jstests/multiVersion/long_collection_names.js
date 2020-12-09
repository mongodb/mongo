/**
 * Tests the FCV for long collection names.
 *
 * On 4.2 and below, the maximum fully qualified collection name is 120 characters or less
 * (which includes the database name).
 *
 * In this multi version test, we ensure that we can create long collection names on the 4.4 binary
 * while the FCV document is set to 4.4. Restarting with long collection names present on a 4.2
 * binary should not crash the server. Users would need to manually remove or rename the long
 * collection names prior to downgrading. Additionally, we should be prevented from creating long
 * collection names when using FCV 4.2 on a 4.4 binary.
 */
(function() {
'use strict';

const dbName = 'test';
const renameDbName = 'rename_test';
const shortCollName = 'short_collection';
const longCollName = 'long_collection' +
    'a'.repeat(200);
const longCollNameRename = 'long_collection' +
    'b'.repeat(200);

const dbpath = MongoRunner.dataPath + 'long_collection_names';
resetDbpath(dbpath);

const mongodOptions42 =
    Object.extend({binVersion: 'last-stable'}, {dbpath: dbpath, cleanData: false});
const mongodOptions44 = Object.extend({binVersion: 'latest'}, {dbpath: dbpath, cleanData: false});

/**
 * Start up with the latest binary and ensure that long collection names can be created while
 * using FCV 4.4.
 */
let conn = MongoRunner.runMongod(mongodOptions44);
assert.neq(null, conn, 'mongod was unable to start with version ' + tojson(mongodOptions44));

let testDb = conn.getDB(dbName);

assert.commandWorked(testDb.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Create two collections, one with a short name and the other with a long name.
assert.commandWorked(testDb.createCollection(shortCollName));
assert.commandWorked(testDb.createCollection(longCollName));

// Rename a short collection name to a long collection name within the same database.
assert.commandWorked(testDb.adminCommand(
    {renameCollection: dbName + '.' + shortCollName, to: dbName + '.' + longCollNameRename}));

assert.eq(true, testDb.getCollection(longCollNameRename).drop());
assert.commandWorked(testDb.createCollection(shortCollName));

// Rename a short collection name to a long collection name in a different database.
assert.commandWorked(testDb.adminCommand(
    {renameCollection: dbName + '.' + shortCollName, to: renameDbName + '.' + longCollNameRename}));

assert.eq(true, testDb.getSiblingDB(renameDbName).getCollection(longCollNameRename).drop());
assert.commandWorked(testDb.createCollection(shortCollName));

MongoRunner.stopMongod(conn);

/**
 * Restarting with a 4.2 binary with FCV 4.4 shouldn't startup nor crash.
 */
let restartOpts42 = Object.extend(mongodOptions42, {restart: true});
conn = MongoRunner.runMongod(restartOpts42);
assert.eq(null, conn, 'mongod was able to start with version ' + tojson(restartOpts42));

/**
 * Cannot downgrade to FCV 4.2 on a 4.4 binary when long collection names are present.
 */
let restartOpts44 = Object.extend(mongodOptions44, {restart: true});
conn = MongoRunner.runMongod(restartOpts44);
assert.neq(null, conn, 'mongod was unable to start with version ' + tojson(restartOpts44));

testDb = conn.getDB(dbName);
assert.commandFailedWithCode(testDb.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}),
                             ErrorCodes.InvalidNamespace);

/**
 * FCV can be set to 4.2 after removing the long collection name. However, we cannot create any new
 * collections with long names in FCV 4.2.
 */
testDb = conn.getDB(dbName);
assert.eq(true, testDb.getCollection(longCollName).drop());

assert.commandWorked(testDb.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// Creating a long collection name on a 4.4 binary with FCV 4.2 should fail.
assert.commandFailedWithCode(testDb.createCollection('c'.repeat(8192)), 4862100);
assert.commandFailedWithCode(testDb.createCollection(longCollName),
                             ErrorCodes.IncompatibleServerVersion);

// Running rename within the same database or across two databases should fail for long collection
// names.
assert.commandFailedWithCode(
    testDb.adminCommand(
        {renameCollection: dbName + '.' + shortCollName, to: dbName + '.' + longCollNameRename}),
    ErrorCodes.IncompatibleServerVersion);
assert.commandFailedWithCode(testDb.adminCommand({
    renameCollection: dbName + '.' + shortCollName,
    to: renameDbName + '.' + longCollNameRename
}),
                             ErrorCodes.IncompatibleServerVersion);

MongoRunner.stopMongod(conn);
})();
