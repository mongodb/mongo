/**
 * Tests that --repair on WiredTiger correctly and gracefully handles a missing _mdb_catalog when
 * a clustered collection exists on the server instance.
 *
 * @tags: [requires_wiredtiger]
 */
(function() {

load('jstests/disk/libs/wt_file_helper.js');
load("jstests/libs/collection_drop_recreate.js");

const dbName = jsTestName();
const collName = "test";
const dbpath = MongoRunner.dataPath + dbName + "/";

const runRepairTest = function runRepairTestOnMongoDInstance(
    collectionOptions, docToInsert, isTimeseries) {
    let mongod = startMongodOnExistingPath(dbpath);
    let db = mongod.getDB(dbName);

    assertDropCollection(db, collName);
    assertCreateCollection(db, collName, collectionOptions);

    let testColl = db[collName];
    let testCollUri = getUriForColl(testColl);
    let testCollFile = dbpath + testCollUri + ".wt";

    assert.commandWorked(testColl.insert(docToInsert));

    // A document repaired from a timeseries collection will be in a different format than the
    // original document. This is because the timeseries's system.views collection will be not be
    // associated with the orphaned clustered collection. Thus, the data will show up as it would
    // have in the raw system.buckets collection for the timeseries collection.
    const expectedOrphanDoc =
        isTimeseries ? db["system.buckets." + collName].findOne() : testColl.findOne();

    MongoRunner.stopMongod(mongod);

    // Delete the _mdb_catalog.
    let mdbCatalogFile = dbpath + "_mdb_catalog.wt";
    jsTestLog("deleting catalog file: " + mdbCatalogFile);
    removeFile(mdbCatalogFile);

    assertRepairSucceeds(dbpath, mongod.port);

    // Verify that repair succeeds in creating an empty catalog and MongoDB starts up normally with
    // no data.
    mongod = startMongodOnExistingPath(dbpath);
    db = mongod.getDB(dbName);
    testColl = db[collName];
    assert.isnull(testColl.exists());
    assert.eq(testColl.find(docToInsert).itcount(), 0);
    assert.eq(testColl.count(), 0);

    // Ensure the orphaned collection is valid and the document is preserved.
    const orphanedImportantCollName = "orphan." + testCollUri.replace(/-/g, "_");
    const localDb = mongod.getDB("local");
    orphanedCollection = localDb[orphanedImportantCollName];
    assert(orphanedCollection.exists());
    assert.eq(orphanedCollection.count(expectedOrphanDoc),
              1,
              `Expected to find document ${tojson(expectedOrphanDoc)} but collection has contents ${
                  tojson(orphanedCollection.find().toArray())}`);

    const validateResult = orphanedCollection.validate();
    assert(validateResult.valid);
    MongoRunner.stopMongod(mongod);
};

// Standard clustered collection test.
let isTimeseries = false;
let clusteredCollOptions = {clusteredIndex: {key: {_id: 1}, unique: true}};
let docToInsert = {_id: 1};
runRepairTest(clusteredCollOptions, docToInsert, isTimeseries);

// Timeseries test since all timeseries collections are implicitly clustered.
isTimeseries = true;
clusteredCollOptions = {
    timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}
};
docToInsert = {
    "metadata": {"sensorId": 5578, "type": "temperature"},
    "timestamp": ISODate("2021-05-18T00:00:00.000Z"),
    "temp": 12
};
runRepairTest(clusteredCollOptions, docToInsert, isTimeseries);
})();
