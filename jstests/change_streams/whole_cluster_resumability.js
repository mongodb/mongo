// Basic tests for resuming a $changeStream that is open against all databases in a cluster.
import {assertDropAndRecreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

// Create two databases, with one collection in each.
const testDBs = [db.getSiblingDB(jsTestName()), db.getSiblingDB(jsTestName() + "_other")];
let [db1Coll, db2Coll] = testDBs.map((db) => assertDropAndRecreateCollection(db, "test"));
const adminDB = db.getSiblingDB("admin");

let cst = new ChangeStreamTest(adminDB);
let resumeCursor = cst.startWatchingAllChangesForCluster();

// Insert a document in the first database and save the resulting change stream.
assert.commandWorked(db1Coll.insert({_id: 1}));
const firstInsertChangeDoc = cst.getOneChange(resumeCursor);
assert.docEq({_id: 1}, firstInsertChangeDoc.fullDocument);

// Test resume after the first insert.
resumeCursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id, allChangesForCluster: true}}],
    collection: 1,
    aggregateOptions: {cursor: {batchSize: 0}},
});

// Write the next document into the second database.
assert.commandWorked(db2Coll.insert({_id: 2}));
const secondInsertChangeDoc = cst.getOneChange(resumeCursor);
assert.docEq({_id: 2}, secondInsertChangeDoc.fullDocument);

// Write the third document into the first database again.
assert.commandWorked(db1Coll.insert({_id: 3}));
const thirdInsertChangeDoc = cst.getOneChange(resumeCursor);
assert.docEq({_id: 3}, thirdInsertChangeDoc.fullDocument);

// Test resuming after the first insert again.
resumeCursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id, allChangesForCluster: true}}],
    collection: 1,
    aggregateOptions: {cursor: {batchSize: 0}},
});
assert.docEq(secondInsertChangeDoc, cst.getOneChange(resumeCursor));
assert.docEq(thirdInsertChangeDoc, cst.getOneChange(resumeCursor));

// Test resume after second insert.
resumeCursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: secondInsertChangeDoc._id, allChangesForCluster: true}}],
    collection: 1,
    aggregateOptions: {cursor: {batchSize: 0}},
});
assert.docEq(thirdInsertChangeDoc, cst.getOneChange(resumeCursor));

// Rename the collection and obtain a resume token from the 'rename' notification. Skip this
// test when running on a sharded collection, since these cannot be renamed.
if (!FixtureHelpers.isSharded(db1Coll)) {
    assertDropAndRecreateCollection(db1Coll.getDB(), db1Coll.getName());
    const renameColl = db1Coll.getDB().getCollection("rename_coll");
    assertDropCollection(renameColl.getDB(), renameColl.getName());

    resumeCursor = cst.startWatchingChanges({
        collection: 1,
        pipeline: [{$changeStream: {allChangesForCluster: true}}],
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.commandWorked(db1Coll.renameCollection(renameColl.getName()));

    const renameChanges = cst.assertNextChangesEqual({
        cursor: resumeCursor,
        expectedChanges: [
            {
                operationType: "rename",
                ns: {db: db1Coll.getDB().getName(), coll: db1Coll.getName()},
                to: {db: renameColl.getDB().getName(), coll: renameColl.getName()},
            },
        ],
    });
    const resumeTokenRename = renameChanges[0]._id;

    // Insert into the renamed collection.
    assert.commandWorked(renameColl.insert({_id: "after rename"}));

    // Resume from the rename notification using 'resumeAfter' and verify that the change stream
    // returns the next insert.
    let expectedInsert = {
        operationType: "insert",
        ns: {db: renameColl.getDB().getName(), coll: renameColl.getName()},
        fullDocument: {_id: "after rename"},
        documentKey: {_id: "after rename"},
    };
    resumeCursor = cst.startWatchingChanges({
        collection: 1,
        pipeline: [{$changeStream: {resumeAfter: resumeTokenRename, allChangesForCluster: true}}],
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

    // Resume from the rename notification using 'startAfter' and verify that the change stream
    // returns the next insert.
    expectedInsert = {
        operationType: "insert",
        ns: {db: renameColl.getDB().getName(), coll: renameColl.getName()},
        fullDocument: {_id: "after rename"},
        documentKey: {_id: "after rename"},
    };
    resumeCursor = cst.startWatchingChanges({
        collection: 1,
        pipeline: [{$changeStream: {startAfter: resumeTokenRename, allChangesForCluster: true}}],
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

    // Rename back to the original collection for reliability of the collection drops when
    // dropping the database.
    assert.commandWorked(renameColl.renameCollection(db1Coll.getName()));
}

// Dropping a database should generate a 'drop' notification for the collection followed by a
// 'dropDatabase' notification.
resumeCursor = cst.startWatchingAllChangesForCluster();
assert.commandWorked(testDBs[0].dropDatabase());
const dropDbChanges = cst.assertDatabaseDrop({cursor: resumeCursor, db: testDBs[0]});
const resumeTokenDbDrop = dropDbChanges[dropDbChanges.length - 1]._id;

// Recreate the collection and insert a document.
assert.commandWorked(db1Coll.insert({_id: "after recreate"}));

let expectedInsert = {
    operationType: "insert",
    ns: {db: testDBs[0].getName(), coll: db1Coll.getName()},
    fullDocument: {_id: "after recreate"},
    documentKey: {_id: "after recreate"},
};

// Resume from the database drop using 'resumeAfter', and verify the change stream picks up
// the insert.
resumeCursor = cst.startWatchingChanges({
    collection: 1,
    pipeline: [{$changeStream: {resumeAfter: resumeTokenDbDrop, allChangesForCluster: true}}],
    aggregateOptions: {cursor: {batchSize: 0}},
});
cst.consumeDropUpTo({
    cursor: resumeCursor,
    dropType: "dropDatabase",
    expectedNext: expectedInsert,
});

// Resume from the database drop using 'startAfter', and verify the change stream picks up the
// insert.
resumeCursor = cst.startWatchingChanges({
    collection: 1,
    pipeline: [{$changeStream: {startAfter: resumeTokenDbDrop, allChangesForCluster: true}}],
    aggregateOptions: {cursor: {batchSize: 0}},
});
cst.consumeDropUpTo({
    cursor: resumeCursor,
    dropType: "dropDatabase",
    expectedNext: expectedInsert,
});

cst.cleanUp();
