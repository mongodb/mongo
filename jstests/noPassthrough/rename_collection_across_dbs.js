/**
 * Tests batching of insert oplog entries when a collection is renamed across databases.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

// Declaring all variables up top so each test can be individually run
let srcNsRegex, opEntries, applyOpsOpEntries, db, db2, coll1, primary, numNewDocs, testDescription,
    largeNumberOfDocsToExceedBatchCountLimit, srcDb, dstDb, srcColl, testNum, numDocsToInsert;

const testName = jsTestName();
let dbName1 = testName + '1-1';
let dbName2 = testName + '1-2';
const BSONObjMaxUserSize = 16 * 1024 * 1024;
const collName = "test";
const collCount = 10;
const maxInsertsCount = collCount;
const maxInsertsSize = /*single object size=*/ 14 * maxInsertsCount * 3;
let srcNs = dbName1 + '.' + collName;
let dstNs = dbName2 + '.' + collName;

//
// 1. Multiple oplog entries
//
jsTestLog("1. Multiple oplog entries");
const setParams = {
    "maxNumberOfInsertsBatchInsertsForRenameAcrossDatabases": maxInsertsCount,
    "maxSizeOfBatchedInsertsForRenameAcrossDatabasesBytes": maxInsertsSize
};
let rst = new ReplSetTest(
    {nodes: [{}, {rsConfig: {votes: 0, priority: 0}}], nodeOptions: {setParameter: setParams}});
rst.startSet();
rst.initiate();

primary = rst.getPrimary();
db = primary.getDB(dbName1);

coll1 = db.getCollection(collName);
assert.commandWorked(coll1.insertMany([...Array(collCount).keys()].map(x => ({_id: x, a: x}))));
assert.commandWorked(db.adminCommand({renameCollection: srcNs, to: dstNs}));

db2 = primary.getDB(dbName2);
opEntries =
    db.getSiblingDB('local')
        .oplog.rs.find({op: 'i', ns: {$regex: '^' + dbName2 + '\.tmp.{5}\.renameCollection$'}})
        .sort({$natural: -1})
        .toArray();

assert.eq(0, opEntries.length, "Should be no insert oplog entries.");

opEntries = db.getSiblingDB('local')
                .oplog.rs
                .find({op: 'c', ns: 'admin.$cmd', "o.applyOps": {$exists: true}}, {"o.applyOps": 1})
                .sort({$natural: -1})
                .toArray();

applyOpsOpEntries = opEntries[0].o.applyOps;
for (const op of applyOpsOpEntries) {
    assert.eq(op.op, "i", "Each applyOps operation should be an insert");
}
assert.eq(
    collCount, applyOpsOpEntries.length, "Should have " + collCount + " applyOps insert ops.");

// Check prior collection gone
assert(!db.getCollectionNames().includes(collName));

// Check new collection exists
numNewDocs = db2.getCollection(collName).find().itcount();
assert.eq(collCount, numNewDocs);

//
// 2. Single oplog entry
//
jsTestLog("2. Single oplog entry");
dbName1 = testName + '2-1';
dbName2 = testName + '2-2';
srcNs = dbName1 + '.' + collName;
dstNs = dbName2 + '.' + collName;

primary = rst.getPrimary();
db = primary.getDB(dbName1);
db2 = primary.getDB(dbName1);

coll1 = db.getCollection(collName);
assert.commandWorked(coll1.insertOne(({_id: 1, a: 1})));
assert.commandWorked(db.adminCommand({renameCollection: srcNs, to: dstNs}));
opEntries =
    db.getSiblingDB('local')
        .oplog.rs.find({op: 'i', ns: {$regex: '^' + dbName2 + '\.tmp.{5}\.renameCollection$'}})
        .sort({$natural: -1})
        .toArray();
assert.eq(1, opEntries.length, "Should be 1 insert oplog entry, not in applyOps format.");
opEntries = db.getSiblingDB('local')
                .oplog.rs
                .find({
                    op: 'c',
                    ns: 'admin.$cmd',
                    "o.applyOps": {$exists: true},
                    "o.applyOps.ns": {$regex: '^' + dbName2 + '\.tmp.{5}\.renameCollection$'}
                })
                .sort({$natural: -1})
                .toArray();
assert.eq(
    0, opEntries.length, "Should be no applyOps oplog entries for an unbatched single insert.");

//
// 3. Too many documents
//
jsTestLog("3. Too many documents");
dbName1 = testName + '3-1-too-many-documents';
dbName2 = testName + '3-2-too-many-documents';
srcNs = dbName1 + '.' + collName;
dstNs = dbName2 + '.' + collName;

primary = rst.getPrimary();
db = primary.getDB(dbName1);
db2 = primary.getDB(dbName1);

coll1 = db.getCollection(collName);
largeNumberOfDocsToExceedBatchCountLimit = (maxInsertsCount * 4);

// Add 1 extra document to spill over to a new batch with 1 element
for (let i = 0; i < largeNumberOfDocsToExceedBatchCountLimit + 1; i++) {
    assert.commandWorked(coll1.insertOne(({a: i})));
}

assert.commandWorked(db.adminCommand({renameCollection: srcNs, to: dstNs}));

// ns: rename_collection_across_dbs3-2-too-many-documents.tmp1h5Aj.renameCollection
opEntries =
    db.getSiblingDB('local')
        .oplog.rs.find({op: 'i', ns: {$regex: '^' + dbName2 + '\.tmp.{5}\.renameCollection$'}})
        .sort({$natural: -1})
        .toArray();

assert.eq(opEntries.length, 1, "Spillover insert should have its own insert entry.");

opEntries = db.getSiblingDB('local')
                .oplog.rs
                .find({
                    op: 'c',
                    ns: 'admin.$cmd',
                    "o.applyOps": {$exists: true},
                    "o.applyOps.ns": {$regex: '^' + dbName2 + '\.tmp.{5}\.renameCollection$'}
                })
                .sort({$natural: -1})
                .toArray();

for (let i = 0; i < opEntries.length; i++) {
    for (const op of opEntries[i]["o"]["applyOps"]) {
        assert.eq(op.op, "i", "Each applyOps operation should be an insert.");
    }
    // Keep in mind there is also a limit on the total size of the documents not just
    // the number of documents.
    assert.eq(opEntries[i]['o']["applyOps"].length,
              maxInsertsCount,
              "Each batch should contain the maximum size of " + maxInsertsCount + "entries.");
}
assert.eq(largeNumberOfDocsToExceedBatchCountLimit / maxInsertsCount,
          opEntries.length,
          "Should be " + largeNumberOfDocsToExceedBatchCountLimit + "/" + maxInsertsCount +
              " insert oplog entries, in applyOps format.");

/*
 * 4. Too many documents, in size, testing the defaults
 */
testNum = 4;
jsTestLog(testNum + ". Too many documents, in size, testing the defaults");

let defaultMaxBatchSize, serverParam, sampleDoc, sampleDocFull, sampleDocSize, testCollName,
    insertOps, applyOps;

rst.stopSet();
rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {votes: 0, priority: 0}}],
    nodeOptions: {
        setParameter: {
            maxNumberOfInsertsBatchInsertsForRenameAcrossDatabases:
                BSONObjMaxUserSize,  // ensure size limit is hit, not count
        }
    }
});
rst.startSet();
rst.initiate();

testDescription = "-too-many-documents-size-default";
testCollName = collName + testDescription;
dbName1 = testName + testNum + '-src';
dbName2 = testName + testNum + '-dst';
srcNs = dbName1 + '.' + testCollName;
dstNs = dbName2 + '.' + testCollName;
srcNsRegex = '^' + dbName2 + '\.tmp.{5}\.renameCollection$';

primary = rst.getPrimary();
srcDb = primary.getDB(dbName1);
dstDb = primary.getDB(dbName2);

sampleDoc = {
    x: "a",
    longString: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
};
sampleDocFull = {
    _id: ObjectId(),
    doc: sampleDoc
};
sampleDocSize = Object.bsonsize(sampleDocFull);

// Get default batched size byte limit to verify the current limit
serverParam = assert.commandWorked(
    srcDb.adminCommand({getParameter: 1, maxSizeOfBatchedInsertsForRenameAcrossDatabasesBytes: 1}));
jsTestLog("Default batch size limit for renames across databases:" + tojson(serverParam));

defaultMaxBatchSize = serverParam["maxSizeOfBatchedInsertsForRenameAcrossDatabasesBytes"];
jsTestLog("defaultMaxBatchSize: " + defaultMaxBatchSize);
jsTestLog("sampleDocSize: " + sampleDocSize);
numDocsToInsert = Math.floor(defaultMaxBatchSize / sampleDocSize);

jsTestLog("Inserting [" + numDocsToInsert + "] docs.");
srcColl = srcDb.getCollection(testCollName);

assert.commandWorked(srcColl.insertMany(
    [...Array(numDocsToInsert).keys()].map(x => ({_id: ObjectId(), doc: sampleDoc}))));

// Rename Collection Across Databases
assert.commandWorked(srcDb.adminCommand({renameCollection: srcNs, to: dstNs}));

// Verify operation from oplog
insertOps = rst.findOplog(primary, {op: 'i', ns: {$regex: srcNsRegex}}).toArray();
jsTestLog('plain insert oplog entry num: ' + insertOps.length);
jsTestLog('plain insert oplog entry: ' + tojson(insertOps));

applyOps = rst.findOplog(primary, {
                  op: 'c',
                  ns: 'admin.$cmd',
                  'o.applyOps': {$elemMatch: {op: 'i', ns: {$regex: srcNsRegex}}}
              })
               .toArray();
jsTestLog('applyOps oplog entries num: ' + applyOps.length);
jsTestLog('applyOps oplog entries: ' + tojson(applyOps));
assert.eq(numDocsToInsert,
          applyOps[0].o.applyOps.length,
          "Expect all inserts to land in one applyOps batch.");
assert.eq(0, insertOps.length, "Expect no plain insert ops.");

//
// Teardown
//
rst.stopSet();
