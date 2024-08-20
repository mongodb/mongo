/**
 * Tests that we can remove the 'recordIdsReplicated' flag from
 * a collection's catalog entry via the collMod command.
 *
 * This allows us to irreversibly disable replicated record IDs on a collection.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replSet = new ReplSetTest({nodes: [{}, {rsConfig: {votes: 0, priority: 0}}]});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();

const collName = 'replRecIdCollForCollMod';

// Create a collection with the param set.
const testDB = primary.getDB('test');
testDB.runCommand({create: collName, recordIdsReplicated: true});
const coll = testDB.getCollection(collName);
assert.commandWorked(coll.insert({_id: 1}));

// For the coll the recordId should be in the oplog, and should match
// the actual recordId on disk.
const primOplog = replSet.findOplog(primary, {ns: coll.getFullName(), 'o._id': 1}).toArray()[0];
const doc = coll.find().showRecordId().toArray()[0];
assert.eq(
    primOplog.rid,
    doc["$recordId"],
    `Mismatching recordIds. Primary's oplog entry: ${tojson(primOplog)}, on disk: ${tojson(doc)}`);

// Enable debug logs for catalog changes.
const originalStorageLogLevel =
    assert.commandWorked(testDB.setLogLevel(1, 'storage')).was.storage.verbosity;

let result =
    assert.commandWorked(testDB.runCommand({collMod: collName, recordIdsReplicated: false}));
jsTestLog('Result from successful collMod command: ' + tojson(result));

// Check for "Unsetting 'recordIdsReplicated' catalog entry flag" debug log message.
checkLog.containsJson(primary, 8650601, {namespace: coll.getFullName()});
assert.commandWorked(testDB.setLogLevel(originalStorageLogLevel, 'storage'));

// Confirm that 'recordIdsReplicated' option has been removed from collection options.
const collInfo = coll.exists();
assert(collInfo,
       'unable to find collection ' + coll.getFullName() +
           ' in listCollections results: ' + tojson(testDB.getCollectionInfos()));
jsTestLog('Collection options: ' + tojson(collInfo));
assert(!collInfo.options.hasOwnProperty('recordIdsReplicated'),
       'collMod failed to remove recordIdsReplicated flag from collection options');

// Check collection metadata on secondary.
replSet.awaitReplication();
const secondary = replSet.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
const secondaryColl = secondaryDB.getCollection(coll.getName());
const secondaryCollInfo = secondaryColl.exists();
assert(secondaryCollInfo,
       'unable to find collection ' + secondaryColl.getFullName() +
           ' in listCollections results on secondary: ' + tojson(secondaryDB.getCollectionInfos()));
assert(!secondaryCollInfo.options.hasOwnProperty('recordIdsReplicated'),
       'collMod failed to remove recordIdsReplicated flag from collection options on secondary');

// Running collMod on a collection that does not replicate record IDs is disallowed.
let error = assert.commandFailedWithCode(
    testDB.runCommand({collMod: collName, recordIdsReplicated: false}), 8650600);
jsTestLog('Error from running collMod on a collection that does not replicate record IDs: ' +
          tojson(error));

// Modifying with a true 'recordIdsReplicated' value is not allowed
assert.commandFailedWithCode(testDB.runCommand({collMod: collName, recordIdsReplicated: true}),
                             ErrorCodes.InvalidOptions);

// Insert a document and confirm that the record ID is not replicated in the oplog.
assert.commandWorked(coll.insert({_id: 2}));
const oplogNoRid = replSet.findOplog(primary, {ns: coll.getFullName(), 'o._id': 2}).toArray()[0];
assert(!oplogNoRid.rid, `Unexpectedly found rid in entry: ${tojson(oplogNoRid)}`);

replSet.stopSet();
