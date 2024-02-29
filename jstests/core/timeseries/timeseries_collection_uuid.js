/**
 * Tests using the collectionUUID parameter when operating on a time-series collection.
 *
 * @tags: [
 *   requires_timeseries,
 *   assumes_stable_collection_uuid,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const dbName = jsTestName();
const collName = "coll";
const bucketsCollName = "system.buckets." + collName;

const testDB = db.getSiblingDB(dbName);
testDB.dropDatabase();

assert.commandWorked(
    testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));

assert.commandWorked(testDB.runCommand({
    createIndexes: collName,
    indexes: [{name: "a1", key: {"a": 1}}, {name: "m1", key: {"m": 1}}],
}));
testDB.dropDatabase();

assert.commandWorked(
    testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));

const nonexistentUUID = UUID();
const bucketsCollUUID = testDB.getCollectionInfos({name: bucketsCollName})[0].info.uuid;

const checkResult = function(res, uuid) {
    assert.commandFailedWithCode(res, ErrorCodes.CollectionUUIDMismatch);

    if (res.writeErrors) {
        assert.eq(res.writeErrors.length, 1);
        res = res.writeErrors[0];
    }

    assert.eq(res.db, dbName);
    assert.eq(res.collectionUUID, uuid);
    assert.eq(res.expectedCollection, collName);
    assert.eq(res.actualCollection, uuid === bucketsCollUUID ? bucketsCollName : null);
};

const testInsert = function(uuid, ordered) {
    checkResult(testDB.runCommand({
        insert: collName,
        documents: [{t: ISODate()}],
        collectionUUID: uuid,
        ordered: ordered,
    }),
                uuid);
};

const testCollMod = function(uuid) {
    checkResult(testDB.runCommand({
        collMod: collName,
        collectionUUID: uuid,
    }),
                uuid);
};

const testUpdate = function(uuid, field) {
    assert.commandWorked(testDB[collName].insert({t: ISODate(), m: 1, a: 1}));
    checkResult(testDB.runCommand({
        update: collName,
        updates: [{
            q: {[field]: 1},
            u: {$set: {[field]: 1}},
        }],
        collectionUUID: uuid,
    }),
                uuid);
};

const testCreateIndex = function(uuid, field) {
    checkResult(testDB.runCommand({
        createIndexes: collName,
        indexes: [{name: "indexFieldName", key: {[field]: 1}}],
        collectionUUID: uuid,
    }),
                uuid);
};

const testDelete = function(uuid, field) {
    assert.commandWorked(testDB[collName].insert({t: ISODate(), m: 1, a: 1}));
    checkResult(testDB.runCommand({
        delete: collName,
        deletes: [{
            q: {[field]: 1},
            limit: 1,
        }],
        collectionUUID: uuid,
    }),
                uuid);
};

for (const uuid of [nonexistentUUID, bucketsCollUUID]) {
    testInsert(uuid, true);
    testInsert(uuid, false);

    testCollMod(uuid);

    testCreateIndex(uuid, "m");
    testCreateIndex(uuid, "a");

    if (FeatureFlagUtil.isPresentAndEnabled(testDB, "TimeseriesUpdatesSupport")) {
        testUpdate(uuid, "m");
        testUpdate(uuid, "a");
    }

    testDelete(uuid, "m");
    testDelete(uuid, "a");
}
