// @tags: [
//   requires_persistence,
// ]
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = "query_yields_catch_index_corruption";

const replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();

let db = primary.getDB(dbName);
assert.commandWorked(
    db.adminCommand({
        configureFailPoint: "skipUnindexingDocumentWhenDeleted",
        mode: "alwaysOn",
        data: {indexName: "a_1_b_1"},
    }),
);

let coll = db.getCollection(collName);
assert.commandWorked(db.createCollection(collName, {writeConcern: {w: "majority"}}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

// Corrupt the collection by inserting a document and then deleting it without deleting its index
// entry (thanks to the "skipUnindexingDocumentWhenDeleted" failpoint).
function createDanglingIndexEntry(doc) {
    assert.commandWorked(coll.insert(doc));
    const docId = coll.findOne(doc)._id;
    assert.commandWorked(coll.remove(doc));

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
            jsTestLog("Found oplog entry for corrupted index entry: " + tojson(oplogDoc));
            if (oplogDoc.op === "d") {
                foundDelete = true;
            } else if (oplogDoc.op === "i") {
                foundInsert = true;
            }
            return foundDelete && foundInsert;
        },
    });

    // A query that accesses the now dangling index entry should fail with a
    // "DataCorruptionDetected" error. Most reads will not detect this problem because they ignore
    // prepare conflicts by default and that exempts them from checking this assertion. Only writes
    // and reads in multi-document transactions enforce prepare conflicts and should encounter this
    // assertion.
    assert.commandFailedWithCode(coll.update(doc, {$set: {c: 1}}), ErrorCodes.DataCorruptionDetected);

    const session = db.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    session.startTransaction();

    assert.throwsWithCode(() => {
        sessionDB[collName].find(doc).toArray();
    }, ErrorCodes.DataCorruptionDetected);
    session.abortTransaction_forTesting();

    // Check that reads going through the express path also fail.
    const session2 = db.getMongo().startSession();
    const session2DB = session2.getDatabase(dbName);
    session2.startTransaction();

    assert.throwsWithCode(() => {
        session2DB[collName].findOne({a: 1}).toArray();
    }, ErrorCodes.DataCorruptionDetected);
    session2.abortTransaction_forTesting();
}

createDanglingIndexEntry({a: 1, b: 1});

// Fix the index by rebuilding it, and ensure that it validates.
assert.commandWorked(coll.dropIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

let validateRes = assert.commandWorked(coll.validate());
assert.eq(true, validateRes.valid, tojson(validateRes));

// Reintroduce the dangling index entry, and this time fix it using the "repair" flag.
createDanglingIndexEntry({a: 1, b: 1});

const dbpath = replSet.getDbPath(primary);
replSet.stopSet(MongoRunner.EXIT_CLEAN, true /* forRestart */, {skipValidation: true});

let mongod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true, repair: ""});
assert.eq(null, mongod, "Expect this to exit cleanly");

// Verify that the server starts up successfully after the repair.
mongod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
assert.neq(null, mongod, "mongod failed to start after repair");

db = mongod.getDB("test");
coll = db.getCollection(collName);

// Runs validate before shutting down.
MongoRunner.stopMongod(mongod);
