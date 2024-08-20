/**
 * Verifies that when running validate on a replica set node started as standalone, when corruption
 * is found, the oplog entry is logged.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = "validate_log_oplog_entry_on_standalone";

const replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();

let db = primary.getDB(dbName);
assert.commandWorked(db.adminCommand({
    configureFailPoint: "skipUnindexingDocumentWhenDeleted",
    mode: "alwaysOn",
    data: {indexName: "abcText"}
}));

let coll = db.getCollection(collName);
assert.commandWorked(db.createCollection(collName, {writeConcern: {w: "majority"}}));
assert.commandWorked(coll.createIndex({a: "text", b: "text", c: "text"},
                                      {weights: {a: 5, b: 5, c: 10}, name: "abcText"}));

const doc = {
    a: "a",
    b: "b",
    c: "c"
};
// Corrupt the collection by inserting a document and then deleting it without deleting its index
// entry (thanks to the "skipUnindexingDocumentWhenDeleted" failpoint).
assert.commandWorked(coll.insert(doc));
const docId = coll.findOne(doc)._id;
assert.commandWorked(coll.remove(doc));

// Validate as standalone.

const dbpath = replSet.getDbPath(primary);
replSet.stopSet(MongoRunner.EXIT_CLEAN, true /* forRestart */, {skipValidation: true});

// Restart as standalone.
const mongod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
assert.neq(null, mongod, "mongod failed to start");

db = mongod.getDB("test");
coll = db.getCollection(collName);

// Validation should now fail.
const validateRes = assert.commandWorked(coll.validate());
assert.eq(false, validateRes.valid);

// Server logs for failed validation command should contain oplog entries related to corrupted
// index entry.
let foundInsert = false;
let foundDelete = false;
// Look for log message "Oplog entry found for corrupted collection and index entry" (msg id
// 7462402).
checkLog.containsJson(db.getMongo(), 7464202, {
    oplogEntryDoc: (oplogDoc) => {
        let oplogDocId;
        try {
            oplogDocId = ObjectId(oplogDoc.o._id.$oid);
        } catch (ex) {
            return false;
        }
        if (!oplogDocId.equals(docId)) {
            return false;
        }
        jsTestLog('Found oplog entry for corrupted index entry: ' + tojson(oplogDoc));
        if (oplogDoc.op === 'd') {
            foundDelete = true;
        } else if (oplogDoc.op === 'i') {
            foundInsert = true;
        }
        return foundDelete && foundInsert;
    }
});

// Skip validation.
MongoRunner.stopMongod(mongod, undefined, {skipValidation: true});
