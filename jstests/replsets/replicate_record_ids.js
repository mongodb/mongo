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
const db = primary.getDB('test');
db[unRepRidlNs].insert({_id: 1});

const oplogNoRid = replSet.findOplog(primary, {ns: `test.${unRepRidlNs}`}).toArray()[0];
assert(!oplogNoRid.rid, `Unexpectedly found rid in entry: ${tojson(oplogNoRid)}`);

// Create a collection with the param set. This time the recordId should show up
// in the oplog.
db.runCommand({create: replRidNs, recordIdsReplicated: true});
db[replRidNs].insert({_id: 1});

// For the replRecIdColl the recordId should be in the oplog, and should match
// the actual recordId on disk.
const primOplog = replSet.findOplog(primary, {ns: `test.${replRidNs}`}).toArray()[0];
const secOplog = replSet.findOplog(secondary, {ns: `test.${replRidNs}`}).toArray()[0];
const doc = db.replRecIdColl.find().showRecordId().toArray()[0];
assert.eq(
    primOplog.rid,
    doc["$recordId"],
    `Mismatching recordIds. Primary's oplog entry: ${tojson(primOplog)}, on disk: ${tojson(doc)}`);
assert.eq(secOplog.rid,
          doc["$recordId"],
          `Mismatching recordIds. Secondary's oplog entry: ${tojson(primOplog)}, on disk: ${
              tojson(doc)}`);

replSet.stopSet();
