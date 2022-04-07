/**
 * Tests that a unique index built or converted to in the latest version are in the new data
 * formats. Unique indexes built on older versions will stay the same data formats.
 * @tags: [
 *   future_git_tag_incompatible,
 * ]
 */

(function() {
"use strict";

const dbpath = MongoRunner.dataPath + 'unique_index_data_format';

const lastLTSVersion = "last-lts";
const lastContinuous = "last-continuous";
const latestVersion = "latest";

const collNamePrefix = 'unique_index_data_format_';

function checkIndexVersion(coll, key, expectedVersion) {
    const index = coll.getIndexes().find(function(idx) {
        return friendlyEqual(idx.key, key);
    });

    const indexDataFormat = coll.aggregate({$collStats: {storageStats: {}}})
                                .next()
                                .storageStats.indexDetails[index.name]
                                .metadata.formatVersion;

    assert.eq(indexDataFormat,
              expectedVersion,
              "Expected index format version " + expectedVersion + " for index: " + tojson(index));
}

function convertToUnique(db, collName, key) {
    assert.commandWorked(
        db.runCommand({collMod: collName, index: {keyPattern: key, prepareUnique: true}}));
    assert.commandWorked(
        db.runCommand({collMod: collName, index: {keyPattern: key, unique: true}}));
}

function testIndexVersionsOverUpgrade(fromVersion) {
    resetDbpath(dbpath);
    const collName = collNamePrefix + fromVersion;
    jsTestLog("Builds unique indexes in both index versions on " + fromVersion + ".");
    {
        const mongodOption = {binVersion: fromVersion, dbpath: dbpath, noCleanData: true};
        const conn = MongoRunner.runMongod(mongodOption);
        const db = conn.getDB('test');
        const coll = db.getCollection(collName);
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1}, {v: 1, unique: true}));
        assert.commandWorked(coll.createIndex({b: 1}, {unique: true}));
        checkIndexVersion(coll, {a: 1}, 11);
        checkIndexVersion(coll, {b: 1}, 12);
        MongoRunner.stopMongod(conn);
    }

    jsTestLog("Upgrades to the latest version.");
    {
        const mongodOption = {binVersion: latestVersion, dbpath: dbpath, noCleanData: true};
        const conn = MongoRunner.runMongod(mongodOption);
        const db = conn.getDB('test');
        const coll = db.getCollection(collName);
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
        // Builds indexes on the latest version and checks they are in the new data formats. The
        // indexes built on older MongoDB versions stay the same.
        assert.commandWorked(coll.createIndex({c: 1}, {v: 1, unique: true}));
        assert.commandWorked(coll.createIndex({d: 1}, {unique: true}));
        assert.commandWorked(coll.createIndex({e: 1}, {v: 1}));
        assert.commandWorked(coll.createIndex({f: 1}));
        checkIndexVersion(coll, {a: 1}, 11);
        checkIndexVersion(coll, {b: 1}, 12);
        checkIndexVersion(coll, {c: 1}, 13);
        checkIndexVersion(coll, {d: 1}, 14);
        checkIndexVersion(coll, {e: 1}, 6);
        checkIndexVersion(coll, {f: 1}, 8);

        // Converts the non-unique indexes to unique and checks they are in the new data formats.
        // For unique indexes in the old data format, the conversion should be a no-op and the
        // formats should stay the same.
        convertToUnique(db, collName, {a: 1});
        convertToUnique(db, collName, {b: 1});
        convertToUnique(db, collName, {e: 1});
        convertToUnique(db, collName, {f: 1});
        checkIndexVersion(coll, {a: 1}, 11);
        checkIndexVersion(coll, {b: 1}, 12);
        checkIndexVersion(coll, {e: 1}, 13);
        checkIndexVersion(coll, {f: 1}, 14);
        MongoRunner.stopMongod(conn);
    }
}

testIndexVersionsOverUpgrade(lastLTSVersion);
testIndexVersionsOverUpgrade(lastContinuous);
})();
