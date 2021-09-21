/**
 * Regression test validating that on 5.0, documents in a clustered collections whose RecordId
 * exceeds 127 bytes return an error code on access.
 */

(function() {
'use strict';

const dbpath = MongoRunner.dataPath + 'clustered_collection';
resetDbpath(dbpath);

const defaultOptions = {
    dbpath: dbpath,
    noCleanData: true
};

const kCollName = 'system.buckets.clusteredColl';

let mongodOptions5dot0 = Object.extend({binVersion: '5.0'}, defaultOptions);
let mongodOptions5dot1 = Object.extend({binVersion: '5.1'}, defaultOptions);

// Create a clustered collection in 5.1 and insert a RecordId > 127 bytes
// Then downgrade to 5.0 and validate the RecordId cannot be decoded as too large

jsTestLog("Starting version: 5.1");
let conn = MongoRunner.runMongod(mongodOptions5dot1);
assert.neq(
    null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptions5dot1));

let db = conn.getDB('test');
assert.commandWorked(db.createCollection(kCollName, {clusteredIndex: true}));
assert.commandWorked(db[kCollName].createIndex({a: 1}));
// 126 characters + kStringLike CType + NULL terminator == 128 bytes
assert.commandWorked(db[kCollName].insertOne({_id: 'x'.repeat(126), a: 1}));
assert.eq(1, db[kCollName].find({a: 1}).itcount());

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "5.0"}));
MongoRunner.stopMongod(conn);

jsTestLog("Starting version: 5.0");
conn = MongoRunner.runMongod(mongodOptions5dot0);
assert.neq(
    null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptions5dot0));

db = conn.getDB('test');
assert.commandFailedWithCode(db.runCommand({find: kCollName, filter: {a: 1}}), 5577900);
assert.commandWorked(db[kCollName].insertOne({_id: 'x'.repeat(12), a: 2}));

let stopOptions = {skipValidation: true};
MongoRunner.stopMongod(conn, 15, stopOptions);

jsTestLog("Starting version: 5.1");
conn = MongoRunner.runMongod(mongodOptions5dot1);
assert.neq(
    null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptions5dot1));

db = conn.getDB('test');
assert.eq(1, db[kCollName].find({a: 1}).itcount());
assert.eq(1, db[kCollName].find({a: 2}).itcount());
MongoRunner.stopMongod(conn);
})();
