/**
 * Tests using the collectionUUID parameter when operating on a time-series collection.
 *
 * @tags: [
 *   requires_timeseries,
 *   assumes_stable_collection_uuid,
 * ]
 */

import {
    areViewlessTimeseriesEnabled,
    getTimeseriesCollForDDLOps
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const dbName = jsTestName();
const collName = "coll";

const testDB = db.getSiblingDB(dbName);
testDB.dropDatabase();
const coll = testDB.getCollection(collName);

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

// TODO SERVER-105742 simplify once 9.0 becomes last LTS
// For legacy "viewful" timeseries: timeseriesCollUUID === undefined, bucketsCollUUID !== undefined
// For viewless timeseries: timeseriesCollUUID !== undefined, timeseriesCollUUID === bucketsCollUUID
const timeseriesCollUUID = coll.getUUID();
const bucketsColl = getTimeseriesCollForDDLOps(db, coll);
const bucketsCollUUID = bucketsColl.getUUID();

const checkResult = function(res, uuid) {
    if (bsonBinaryEqual(uuid, timeseriesCollUUID)) {
        assert.commandWorked(res);
        return;
    }

    assert.commandFailedWithCode(res, ErrorCodes.CollectionUUIDMismatch);

    if (res.writeErrors) {
        assert.eq(res.writeErrors.length, 1);
        res = res.writeErrors[0];
    }

    assert.eq(res.db, dbName);
    assert.eq(res.collectionUUID, uuid);
    assert.eq(res.expectedCollection, collName);
    assert.eq(res.actualCollection,
              bsonBinaryEqual(uuid, bucketsCollUUID) ? bucketsColl.getName() : null);
};

const testInsert = function(uuid, ordered) {
    // TODO(SERVER-105501): Insert into a viewless timeseries by its correct UUID should work
    if (bsonBinaryEqual(uuid, timeseriesCollUUID)) {
        assert(areViewlessTimeseriesEnabled(db));
        return;
    }

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
        indexes: [{name: "indexFieldName" + field, key: {[field]: 1}}],
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
