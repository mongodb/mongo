/**
 * Tests batching of insert oplog entries when a collection is renamed across databases.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

(function() {
'use strict';

const testName = jsTestName();
let dbName1 = testName + '1-1';
let dbName2 = testName + '1-2';
const collName = "test";
const collCount = 10;
const maxInsertsCount = collCount;
const maxInsertsSize = /*single object size=*/14 * maxInsertsCount * 3;
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

let primary = rst.getPrimary();
let db = primary.getDB(dbName1);

let coll1 = db.getCollection(collName);
assert.commandWorked(coll1.insertMany([...Array(collCount).keys()].map(x => ({_id: x, a: x}))));
assert.commandWorked(db.adminCommand({renameCollection: srcNs, to: dstNs}));

let db2 = primary.getDB(dbName2);
let opEntries =
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

let applyOpsOpEntries = opEntries[0].o.applyOps;
for (const op of applyOpsOpEntries) {
    assert.eq(op.op, "i", "Each applyOps operation should be an insert");
}
assert.eq(
    collCount, applyOpsOpEntries.length, "Should have " + collCount + " applyOps insert ops.");

// Check prior collection gone
assert(!db.getCollectionNames().includes(collName));

// Check new collection exists
let numNewDocs = db2.getCollection(collName).find().itcount();
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
let largeNumberOfDocsToExceedBatchCountLimit = (maxInsertsCount * 4);

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

rst.stopSet();
})();
