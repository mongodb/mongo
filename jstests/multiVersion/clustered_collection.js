/**
 * Regression test validating that all general-purpose clustered collections must be dropped
 * before downgrading to 5.0.
 *
 * It also validates that on 5.0, documents in a time series buckets collection whose RecordId
 * exceeds 127 bytes return an error code on access. While there isn't a known use case for
 * large binary string RecordId encoding on time series collection, this test case is useful for
 * validating the RecordId KeyString encoding behaviour across versions.
 */

(function() {
'use strict';

const dbpath = MongoRunner.dataPath + 'clustered_collection';
resetDbpath(dbpath);

const defaultOptions = {
    dbpath: dbpath,
    noCleanData: true
};

const kBucketsCollName = 'system.buckets.clusteredColl';
const kClusteredCollName = 'clusteredColl';

let mongodOptions5dot0 = Object.extend({binVersion: '5.0'}, defaultOptions);
let mongodOptions5dot1 = Object.extend(
    {binVersion: '5.1', setParameter: 'featureFlagClusteredIndexes=true'}, defaultOptions);

// Create a clustered collection in 5.1, then downgrade to 5.0 and validate that the
// server is unable to start up.
// Then upgrade back to 5.1, drop the clustered collection and validate that the 5.0
// server starts up correctly.
function testGeneralPurposeClusteredCollection() {
    // Create clustered collection on 5.1.
    jsTestLog("[general-purpose clustered collection] Starting version: 5.1");
    let conn = MongoRunner.runMongod(mongodOptions5dot1);
    assert.neq(
        null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptions5dot1));

    let db = conn.getDB('test');
    assert.commandWorked(
        db.createCollection(kClusteredCollName, {clusteredIndex: {key: {_id: 1}, unique: true}}));

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "5.0"}));
    MongoRunner.stopMongod(conn);

    // Expect 5.0 to fail due to the presence of a clustered collection.
    jsTestLog("[general-purpose clustered collection] Starting version: 5.0");
    assert.throws(
        () => MongoRunner.runMongod(mongodOptions5dot0), [], "The server unexpectedly started");

    // Restart on 5.1, drop the clustered collection.
    jsTestLog("[general-purpose clustered collection] Starting version: 5.1");
    conn = MongoRunner.runMongod(mongodOptions5dot1);
    assert.neq(
        null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptions5dot1));

    db = conn.getDB('test');
    assert.commandWorked(db.runCommand({drop: kClusteredCollName}));
    MongoRunner.stopMongod(conn);

    // Expect 5.0 to start up successfully as there's no clustered collection around.
    jsTestLog("[general-purpose clustered collection] Starting version: 5.0");
    conn = MongoRunner.runMongod(mongodOptions5dot0);
    assert.neq(
        null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptions5dot0));
    MongoRunner.stopMongod(conn);
}

// Create a time series buckets collection in 5.1 and insert a RecordId > 127 bytes
// Then downgrade to 5.0 and validate the RecordId cannot be decoded as too large.
// Then upgrade back to 5.1 and validate correct operation.
function testTimeSeriesBucketsCollection() {
    jsTestLog("[time series buckets collection] Starting version: 5.1");
    let conn = MongoRunner.runMongod(mongodOptions5dot1);
    assert.neq(
        null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptions5dot1));

    let db = conn.getDB('test');
    assert.commandWorked(db.createCollection(kBucketsCollName, {clusteredIndex: true}));
    assert.commandWorked(db[kBucketsCollName].createIndex({a: 1}));
    // 126 characters + kStringLike CType + NULL terminator == 128 bytes
    assert.commandWorked(db[kBucketsCollName].insertOne({_id: 'x'.repeat(126), a: 1}));
    assert.eq(1, db[kBucketsCollName].find({a: 1}).itcount());

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "5.0"}));
    MongoRunner.stopMongod(conn);

    jsTestLog("[time series buckets collection] Starting version: 5.0");
    conn = MongoRunner.runMongod(mongodOptions5dot0);
    assert.neq(
        null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptions5dot0));

    db = conn.getDB('test');
    assert.commandFailedWithCode(db.runCommand({find: kBucketsCollName, filter: {a: 1}}), 5577900);
    assert.commandWorked(db[kBucketsCollName].insertOne({_id: 'x'.repeat(12), a: 2}));

    let stopOptions = {skipValidation: true};
    MongoRunner.stopMongod(conn, 15, stopOptions);

    jsTestLog("[time series buckets collection] Starting version: 5.1");
    conn = MongoRunner.runMongod(mongodOptions5dot1);
    assert.neq(
        null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptions5dot1));

    db = conn.getDB('test');
    assert.eq(1, db[kBucketsCollName].find({a: 1}).itcount());
    assert.eq(1, db[kBucketsCollName].find({a: 2}).itcount());
    MongoRunner.stopMongod(conn);
}

testGeneralPurposeClusteredCollection();
testTimeSeriesBucketsCollection();
})();
