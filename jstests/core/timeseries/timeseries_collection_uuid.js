/**
 * Tests using the collectionUUID parameter when operating on a time-series collection.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

const dbName = jsTestName();
const collName = "coll";

const testDB = db.getSiblingDB(dbName);
testDB.dropDatabase();

assert.commandWorked(
    testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
const coll = testDB[collName];
const bucketsColl = testDB["system.buckets." + collName];

const nonexistentUUID = UUID();
const bucketsCollUUID = testDB.getCollectionInfos({name: bucketsColl.getName()})[0].info.uuid;

const testInsert = function(uuid, ordered) {
    assert.commandFailedWithCode(testDB.runCommand({
        insert: collName,
        documents: [{t: ISODate()}],
        collectionUUID: uuid,
        ordered: ordered,
    }),
                                 ErrorCodes.CollectionUUIDMismatch);
};

testInsert(nonexistentUUID, true);
testInsert(nonexistentUUID, false);
testInsert(bucketsCollUUID, true);
testInsert(bucketsCollUUID, false);
