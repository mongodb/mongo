/**
 * Tests recordIds show up when inserting into a collection with the
 * 'recordIdsReplicated' flag set.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 * ]
 */
const replSet = new ReplSetTest({nodes: 2});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondaries()[0];

const unRepRidlNs = 'unreplRecIdColl';
const replRidNs = 'replRecIdColl';

// Create a collection without the 'recordIdsReplicated' param set. This shouldn't
// insert the recordId (rid) into the oplog.
const primDB = primary.getDB('test');
const secDB = secondary.getDB('test');
primDB[unRepRidlNs].insert({_id: -1});

const oplogNoRid = replSet.findOplog(primary, {ns: `test.${unRepRidlNs}`}).toArray()[0];
assert(!oplogNoRid.rid, `Unexpectedly found rid in entry: ${tojson(oplogNoRid)}`);

// Create a collection with the param set. This time the recordId should show up
// in the oplog.
primDB.runCommand({create: replRidNs, recordIdsReplicated: true});
primDB[replRidNs].insert({_id: -1});

// For the replRecIdColl the recordId should be in the oplog, and should match
// the actual recordId on disk.
const primOplog = replSet.findOplog(primary, {ns: `test.${replRidNs}`}).toArray()[0];
const secOplog = replSet.findOplog(secondary, {ns: `test.${replRidNs}`}).toArray()[0];
const primDoc = primDB[replRidNs].find().showRecordId().toArray()[0];
const secDoc = secDB[replRidNs].find().showRecordId().toArray()[0];
assert.eq(primOplog.rid,
          primDoc["$recordId"],
          `Mismatching recordIds. Primary's oplog entry: ${tojson(primOplog)}, on disk: ${
              tojson(primDoc)}`);
assert.eq(secOplog.rid,
          secDoc["$recordId"],
          `Mismatching recordIds. Secondary's oplog entry: ${tojson(primOplog)}, on disk: ${
              tojson(secDoc)}`);
assert.eq(primOplog.rid,
          secOplog.rid,
          `Mismatching recordIds between primary and secondary. On primary: ${
              tojson(primOplog)}. On secondary: ${tojson(secOplog)}`)

// On replication, secondaries apply oplog entries in parallel - a batch of oplog entries is
// distributed amongst several appliers, who apply the entries in parallel. Therefore, if we
// insert a single document at a time, it is likely that the replicated oplog batches will have
// just a single oplog entry each time, and therefore the secondary will basically be processing
// oplog entries in the same order that they appear on the primary. If processed in the same order,
// it is likely that the secondaries will generate the same recordIds as the primary, even
// with recordIdsReplicated:false.
//
// Therefore to ensure that recordIdsReplicated:true actually works we need to make sure that
// the appliers process oplog entries in parallel, and this is done by having a full batch of
// entries for the appliers to process. We can achieve this by performing an insertMany.
jsTestLog("Test inserting multiple documents at a time.")

const docs = [];
for (let i = 0; i < 500; i++) {
    docs.push({_id: i});
}
assert.commandWorked(primDB[replRidNs].insertMany(docs));
assert.eq(primDB[replRidNs].count(), 501);

// Ensure that the on disk data on both nodes has the same recordIds.
let primCursor = primDB[replRidNs].find().sort({_id: 1}).showRecordId();
let secCursor = secDB[replRidNs].find().sort({_id: 1}).showRecordId();

assert.eq({docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []},
          DataConsistencyChecker.getDiff(primCursor, secCursor));

replSet.stopSet();
