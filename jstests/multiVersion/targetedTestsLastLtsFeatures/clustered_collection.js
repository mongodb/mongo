/**
 * Regression test validating that all general-purpose clustered collections must be dropped
 * before downgrading to 5.0.
 *
 * It also validates that on 5.0, documents in a time series buckets collection whose RecordId
 * exceeds 127 bytes return an error code on access. While there isn't a known use case for
 * large binary string RecordId encoding on time series collection, this test case is useful for
 * validating the RecordId KeyString encoding behaviour across versions.
 *
 * @tags: [future_git_tag_incompatible]
 *
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
let mongodOptionsLastContinuous =
    Object.extend({binVersion: 'last-continuous', setParameter: 'featureFlagClusteredIndexes=true'},
                  defaultOptions);
let mongodOptionsLatest = Object.extend({binVersion: 'latest'}, defaultOptions);

// Create a clustered collection with the server running version 'from', then downgrade to
// 5.0 and validate that the server is unable to start up.
// Then upgrade back to the 'from' version, drop the clustered collection and verify that the 5.0
// server starts up correctly.
function testGeneralPurposeClusteredCollectionDowngradeTo5Dot0(from) {
    // Verify from > '5.0'.
    assert.eq(typeof (from.binVersion), "string");
    assert.eq(true, from.binVersion === "latest" || from.binVersion > "5.0");

    // Start server version 'from' and create a clustered collection.
    jsTestLog("[general-purpose clustered collection] Starting version: " + from.binVersion);
    {
        const conn = MongoRunner.runMongod(from);
        assert.neq(
            null, conn, 'mongod was unable able to start with version ' + tojson(from.binVersion));

        const db = conn.getDB('test');

        if (from.binVersion === "latest") {
            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
        } else if (from.binVersion === "last-continuous") {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));
        } else {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: from.binVersion}));
        }
        assert.commandWorked(db.createCollection(kClusteredCollName,
                                                 {clusteredIndex: {key: {_id: 1}, unique: true}}));

        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "5.0"}));
        MongoRunner.stopMongod(conn);
    }

    // Dowgrade to 5.0 and expect to fail due to the presence of a clustered collection.
    jsTestLog("[general-purpose clustered collection] Starting version: 5.0");
    assert.throws(
        () => MongoRunner.runMongod(mongodOptions5dot0), [], "The server unexpectedly started");

    // Upgrade back to the 'from' version, drop the clustered collection.
    jsTestLog("[general-purpose clustered collection] Starting version: " + from.binVersion);
    {
        const conn = MongoRunner.runMongod(from);
        assert.neq(
            null, conn, 'mongod was unable able to start with version ' + tojson(from.binVersion));

        const db = conn.getDB('test');
        assert.commandWorked(db.runCommand({drop: kClusteredCollName}));
        MongoRunner.stopMongod(conn);
    }

    // Downgrade again to 5.0 and verify we start up successfully as there's no clustered collection
    // around.
    jsTestLog("[general-purpose clustered collection] Starting version: 5.0");
    {
        const conn = MongoRunner.runMongod(mongodOptions5dot0);
        assert.neq(null,
                   conn,
                   'mongod was unable able to start with version ' +
                       tojson(mongodOptions5dot0.binVersion));
        MongoRunner.stopMongod(conn);
    }
}

// Create a clustered collection with the server running version 'from', then downgrade to the
// latest continuous and validate that the server is able to access the clustered collection, and
// similarly that it's possible to upgrade back to that tag.
function testGeneralPurposeClusteredCollectionDowngradeToLatestContinuous(from) {
    // Verify from > '5.x'.
    assert.eq(typeof (from.binVersion), "string");
    assert.eq(true, from.binVersion === "latest" || from.binVersion > "last-continuous");

    // Start server version 'from' and populate a clustered collection.
    jsTestLog("[general-purpose clustered collection] Starting version: " + from.binVersion);
    {
        const conn = MongoRunner.runMongod(from);
        assert.neq(
            null, conn, 'mongod was unable able to start with version ' + tojson(from.binVersion));

        const db = conn.getDB('test');

        const fcv = from.binVersion === 'latest' ? latestFCV : from.binVersion;
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: fcv}));

        assert.commandWorked(db.createCollection(kClusteredCollName,
                                                 {clusteredIndex: {key: {_id: 1}, unique: true}}));
        assert.commandWorked(
            db[kClusteredCollName].insertOne({_id: 'latest', info: "my latest doc"}));

        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));
        MongoRunner.stopMongod(conn);
    }

    // Downgrade to 5.x and verify we're able to read and write the existing clustered collection.
    jsTestLog("[general-purpose clustered collection] Starting version: 5.x");
    {
        const conn = MongoRunner.runMongod(mongodOptionsLastContinuous);
        assert.neq(null,
                   conn,
                   'mongod was unable able to start with version ' +
                       tojson(mongodOptionsLastContinuous.binVersion));
        const db = conn.getDB('test');

        assert.eq(db[kClusteredCollName].findOne({_id: 'latest'})['info'], "my latest doc");
        assert.commandWorked(db[kClusteredCollName].insertOne({_id: '5.x', info: "my 5.x doc"}));
        MongoRunner.stopMongod(conn);
    }

    // Upgrade back to the 'from' version, verify we read documents inserted with both server
    // versions.
    jsTestLog("[general-purpose clustered collection] Starting version: " + from.binVersion);
    {
        const conn = MongoRunner.runMongod(from);
        assert.neq(
            null, conn, 'mongod was unable able to start with version ' + tojson(from.binVersion));

        const db = conn.getDB('test');
        assert.eq(db[kClusteredCollName].findOne({_id: 'latest'})['info'], "my latest doc");
        assert.eq(db[kClusteredCollName].findOne({_id: '5.x'})['info'], "my 5.x doc");

        // Clean up
        assert.commandWorked(db.runCommand({drop: kClusteredCollName}));
        MongoRunner.stopMongod(conn);
    }
}

// Create a time series buckets collection in 5.x and insert a RecordId > 127 bytes
// Then downgrade to 5.0 and validate the RecordId cannot be decoded as too large.
// Then upgrade back to 5.x and validate correct operation.
// This test uses the timeseries 'system.buckets* loophole', which is a way to
// internally access a clustered collection in 5.0 where the feature is not generally
// available.
function testTimeSeriesBucketsCollection() {
    jsTestLog("[time series buckets collection] Starting version: 5.x");
    let conn = MongoRunner.runMongod(mongodOptionsLastContinuous);
    assert.neq(
        null,
        conn,
        'mongod was unable able to start with version ' + tojson(mongodOptionsLastContinuous));

    let db = conn.getDB('test');
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));
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

    jsTestLog("[time series buckets collection] Starting version: 5.x");
    conn = MongoRunner.runMongod(mongodOptionsLastContinuous);
    assert.neq(
        null,
        conn,
        'mongod was unable able to start with version ' + tojson(mongodOptionsLastContinuous));

    db = conn.getDB('test');
    assert.eq(1, db[kBucketsCollName].find({a: 1}).itcount());
    assert.eq(1, db[kBucketsCollName].find({a: 2}).itcount());
    MongoRunner.stopMongod(conn);
}

// 5.x is the last-continuous
// 5.x -> 5.0 downgrade path
testGeneralPurposeClusteredCollectionDowngradeTo5Dot0(mongodOptionsLastContinuous);
// latest -> 5.0 downgrade path
testGeneralPurposeClusteredCollectionDowngradeTo5Dot0(mongodOptionsLatest);
// latest -> 5.x downgrade path
testGeneralPurposeClusteredCollectionDowngradeToLatestContinuous(mongodOptionsLatest);

testTimeSeriesBucketsCollection();
})();
