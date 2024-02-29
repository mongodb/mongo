/**
 * Tests recordIds show up when inserting into a collection with the
 * 'recordIdsReplicated' flag set.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 * ]
 */
import {
    getRidForDoc,
} from "jstests/libs/replicated_record_ids_utils.js";

const replSet = new ReplSetTest({nodes: 2});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondaries()[0];

const dbName = 'test';
const unReplRidCollName = 'unreplRecIdColl';
const replRidCollName = 'replRecIdColl';

const primDB = primary.getDB(dbName);
const secDB = secondary.getDB(dbName);

const unReplRidNs = `${dbName}.${unReplRidCollName}`;
const replRidNs = `${dbName}.${replRidCollName}`;

const getOplogEntries = function(conn, oplogQuery) {
    return replSet.findOplog(conn, oplogQuery).toArray();
};

const validateRidInOplogs = function(oplogQuery, expectedRid) {
    const primOplogEntry = getOplogEntries(primary, oplogQuery)[0];
    const secOplogEntry = getOplogEntries(secondary, oplogQuery)[0];
    assert.eq(primOplogEntry.rid,
              expectedRid,
              `Mismatching recordIds. Primary's oplog entry: ${tojson(primOplogEntry)}, on disk: ${
                  tojson(expectedRid)}. Oplog query ${tojson(oplogQuery)}`);

    assert.eq(secOplogEntry.rid,
              expectedRid,
              `Mismatching recordIds. Secondary's oplog entry: ${tojson(secOplogEntry)}, on disk: ${
                  tojson(expectedRid)}`);
};

const docA = {
    _id: "a"
};

// Create a collection without the 'recordIdsReplicated' param set. This shouldn't
// insert the recordId (rid) into the oplog.
primDB[unReplRidCollName].insert(docA);
const oplogNoRid = replSet.findOplog(primary, {ns: `${unReplRidNs}`}).toArray()[0];
assert(!oplogNoRid.rid, `Unexpectedly found rid in entry: ${tojson(oplogNoRid)}`);

// Create a collection with the param set. This time the recordId should show up
// in the oplog.
primDB.runCommand({create: replRidCollName, recordIdsReplicated: true});
const docAInsertOpTime =
    assert.commandWorked(primDB.runCommand({insert: replRidCollName, documents: [docA]})).opTime;
const docAReplRid = getRidForDoc(primDB, replRidCollName, docA);
assert.eq(docAReplRid, getRidForDoc(secDB, replRidCollName, docA));

// For the replRecIdColl the recordId should be in the oplog, and should match
// the actual recordId on disk.
validateRidInOplogs({ns: `${replRidNs}`, ...docAInsertOpTime}, docAReplRid);

// The recordId should also be in the oplog entry for the delete.
const docARemoveOpTime =
    assert
        .commandWorked(primDB.runCommand({delete: replRidCollName, deletes: [{q: docA, limit: 1}]}))
        .opTime;
validateRidInOplogs({ns: `${replRidNs}`, ...docARemoveOpTime}, docAReplRid);

// On replication, secondaries apply oplog entries in parallel - a batch of oplog entries is
// distributed amongst several appliers, who apply the entries in parallel. Therefore, if we
// insert a single document at a time, it is likely that the replicated oplog batches will have
// just a single oplog entry each time, and therefore the secondary will basically be processing
// oplog entries in the same order that they appear on the primary. If processed in the same
// order, it is likely that the secondaries will generate the same recordIds as the primary,
// even with recordIdsReplicated:false.
//
// Therefore to ensure that recordIdsReplicated:true actually works we need to make sure that
// the appliers process oplog entries in parallel, and this is done by having a full batch of
// entries for the appliers to process. We can achieve this by performing an insertMany.
jsTestLog("Test inserting multiple documents at a time.")

const docs = [];
for (let i = 0; i < 500; i++) {
    docs.push({_id: i});
}
assert.commandWorked(primDB[replRidCollName].insertMany(docs));
assert.eq(primDB[replRidCollName].count(), 500);

// Ensure that the on disk data on both nodes has the same recordIds.
let primCursor = primDB[replRidCollName].find().sort({_id: 1}).showRecordId();
let secCursor = secDB[replRidCollName].find().sort({_id: 1}).showRecordId();

assert.eq({docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []},
          DataConsistencyChecker.getDiff(primCursor, secCursor));

replSet.stopSet();
