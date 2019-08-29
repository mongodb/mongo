/**
 * Tests that the 'ns' field in index specs is removed during metadata changes when in FCV 4.4.
 *
 * Starting in 4.4, the 'ns' field for index specs is no longer generated. We want to ensure that
 * index specs created in prior server versions have their 'ns' field removed when running in FCV
 * 4.4.
 */
(function() {
'use strict';
load('jstests/libs/feature_compatibility_version.js');

const dbName = 'test';
const collName = 'coll';

const dbpath = MongoRunner.dataPath + 'remove_ns_field_in_index_spec';
resetDbpath(dbpath);

const mongodOptions42 =
    Object.extend({binVersion: 'last-stable'}, {dbpath: dbpath, cleanData: false});
const mongodOptions44 = Object.extend({binVersion: 'latest'}, {dbpath: dbpath, cleanData: false});

/**
 * Start up with the 4.2 binary and create a collection. The default '_id' index should have the
 * 'ns' field present in its index spec.
 */
let conn = MongoRunner.runMongod(mongodOptions42);
assert.neq(null, conn, 'mongod was unable to start with version ' + tojson(mongodOptions42));

let testDb = conn.getDB(dbName);
assert.commandWorked(testDb.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
assert.commandWorked(testDb.createCollection(collName));

let coll = testDb.getCollection(collName);
let indexes = coll.getIndexes();

assert.eq(1, indexes.length);
assert.eq(dbName + '.' + collName, indexes[0].ns);

MongoRunner.stopMongod(conn);

/**
 * Restart with the 4.4 binary while in FCV 4.2. The index should not lose its 'ns' field when doing
 * a disk modifying metadata change.
 */
let restartOpts44 = Object.extend(mongodOptions44, {restart: true});
conn = MongoRunner.runMongod(restartOpts44);
assert.neq(null, conn, 'mongod was unable to start with version ' + tojson(restartOpts44));

testDb = conn.getDB(dbName);
coll = testDb.getCollection(collName);

// Run a metadata changing operation.
assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.dropIndex({x: 1}));

indexes = coll.getIndexes();

assert.eq(1, indexes.length);
assert.eq(dbName + '.' + collName, indexes[0].ns);

/**
 * Set the FCV to 4.4. The index should lose its 'ns' field when doing a disk modifying metadata
 * change.
 */
assert.commandWorked(testDb.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Run a metadata changing operation.
assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.dropIndex({x: 1}));

indexes = coll.getIndexes();

assert.eq(1, indexes.length);
assert.eq(false, indexes[0].hasOwnProperty('ns'));

MongoRunner.stopMongod(conn);
})();
