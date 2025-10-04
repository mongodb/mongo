// Tests of metadata notifications for a $changeStream on a whole cluster.
// When run in sharded+transaction passthrough suites, it is possible that the two unsharded
// collections will live on different shards. Majority read concern cannot be off with multi-shard
// transactions, which is why this test needs the tag below.
// @tags: [requires_majority_read_concern]
import {assertDropAndRecreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

// Define two databases. We will conduct our tests by creating one collection in each.
const testDB1 = db.getSiblingDB(jsTestName());
const testDB2 = db.getSiblingDB(jsTestName() + "_other");
const adminDB = db.getSiblingDB("admin");

assert.commandWorked(testDB1.dropDatabase());
assert.commandWorked(testDB2.dropDatabase());

// Create one collection on each database.
let [db1Coll, db2Coll] = [testDB1, testDB2].map((testDB) => assertDropAndRecreateCollection(testDB, "test"));

// Create a ChangeStreamTest on the 'admin' db. Cluster-wide change streams can only be opened
// on admin.
let cst = new ChangeStreamTest(adminDB);
let aggCursor = cst.startWatchingAllChangesForCluster();

// Generate oplog entries of type insert, update, and delete across both databases.
for (let coll of [db1Coll, db2Coll]) {
    assert.commandWorked(coll.insert({_id: 1}));
    assert.commandWorked(coll.update({_id: 1}, {$set: {a: 1}}));
    assert.commandWorked(coll.remove({_id: 1}));
}

// Drop the second database, which should generate a 'drop' entry for the collection followed
// by a 'dropDatabase' entry.
assert.commandWorked(testDB2.dropDatabase());

const changes = {
    [testDB1.getName()]: [],
    [testDB2.getName()]: [],
};

for (let i = 0; i < 6; i++) {
    const change = cst.getOneChange(aggCursor);
    changes[change.ns.db].push(change);
}

// We should get 6 oplog entries; three ops of type insert, update, delete from each database.
for (let expectedDB of [testDB1, testDB2]) {
    const dbChanges = changes[expectedDB.getName()];
    assert.eq(dbChanges[0].operationType, "insert", tojson(changes));
    assert.eq(dbChanges[1].operationType, "update", tojson(changes));
    assert.eq(dbChanges[2].operationType, "delete", tojson(changes));
}
cst.assertDatabaseDrop({cursor: aggCursor, db: testDB2});

// Test that a cluster-wide change stream can be resumed using a token from a collection which
// has been dropped.
db1Coll = assertDropAndRecreateCollection(testDB1, db1Coll.getName());

// Get a valid resume token that the next change stream can use.
aggCursor = cst.startWatchingAllChangesForCluster();

assert.commandWorked(db1Coll.insert({_id: 1}, {writeConcern: {w: "majority"}}));

let change = cst.getOneChange(aggCursor, false);
const resumeToken = change._id;

// For cluster-wide streams, it is possible to resume at a point before a collection is dropped,
// even if the "drop" notification has not been received on the original stream yet.
assertDropCollection(db1Coll, db1Coll.getName());
assert.commandWorked(
    adminDB.runCommand({
        aggregate: 1,
        pipeline: [{$changeStream: {resumeAfter: resumeToken, allChangesForCluster: true}}],
        cursor: {},
    }),
);

// Test that collection drops from any database result in "drop" notifications for the stream.
[db1Coll, db2Coll] = [testDB1, testDB2].map((testDB) => assertDropAndRecreateCollection(testDB, "test"));
let _idForTest = 0;
for (let collToInvalidate of [db1Coll, db2Coll]) {
    // Start watching all changes in the cluster.
    aggCursor = cst.startWatchingAllChangesForCluster();

    let testDB = collToInvalidate.getDB();

    // Insert into the collections on both databases, and verify the change stream is able to
    // pick them up.
    for (let collToWrite of [db1Coll, db2Coll]) {
        assert.commandWorked(collToWrite.insert({_id: _idForTest}));
        change = cst.getOneChange(aggCursor);
        assert.eq(change.operationType, "insert", tojson(change));
        assert.eq(change.documentKey._id, _idForTest);
        assert.eq(change.ns.db, collToWrite.getDB().getName());
        _idForTest++;
    }

    // Renaming the collection should generate a 'rename' notification. Skip this test when
    // running on a sharded collection, since these cannot be renamed.
    if (!FixtureHelpers.isSharded(collToInvalidate)) {
        assertDropAndRecreateCollection(testDB, collToInvalidate.getName());
        const collName = collToInvalidate.getName();

        // Start watching all changes in the cluster.
        aggCursor = cst.startWatchingAllChangesForCluster();
        assert.commandWorked(collToInvalidate.renameCollection("renamed_coll"));
        cst.assertNextChangesEqual({
            cursor: aggCursor,
            expectedChanges: [
                {
                    operationType: "rename",
                    ns: {db: testDB.getName(), coll: collToInvalidate.getName()},
                    to: {db: testDB.getName(), coll: "renamed_coll"},
                },
            ],
        });

        // Repeat the test, this time using the 'dropTarget' option with an existing target
        // collection.
        collToInvalidate = testDB.getCollection("renamed_coll");
        assertDropAndRecreateCollection(testDB, collName);
        assert.commandWorked(testDB[collName].insert({_id: 0}));
        assert.commandWorked(collToInvalidate.renameCollection(collName, true /* dropTarget */));
        cst.assertNextChangesEqual({
            cursor: aggCursor,
            expectedChanges: [
                {
                    operationType: "insert",
                    ns: {db: testDB.getName(), coll: collName},
                    documentKey: {_id: 0},
                    fullDocument: {_id: 0},
                },
                {
                    operationType: "rename",
                    ns: {db: testDB.getName(), coll: "renamed_coll"},
                    to: {db: testDB.getName(), coll: collName},
                },
            ],
        });

        collToInvalidate = testDB[collName];

        // Test renaming a collection to a different database. Do not run this in the mongos
        // passthrough suites since we cannot guarantee the primary shard of the target database
        // and renameCollection requires the source and destination to be on the same shard.
        if (!FixtureHelpers.isMongos(testDB)) {
            const otherDB = testDB.getSiblingDB(testDB.getName() + "_rename_target");
            // Ensure the target database exists.
            const collOtherDB = assertDropAndRecreateCollection(otherDB, "test");
            assertDropCollection(otherDB, collOtherDB.getName());
            aggCursor = cst.startWatchingAllChangesForCluster();
            assert.commandWorked(
                testDB.adminCommand({renameCollection: collToInvalidate.getFullName(), to: collOtherDB.getFullName()}),
            );
            // Do not check the 'ns' field since it will contain the namespace of the temp
            // collection created when renaming a collection across databases.
            change = cst.getOneChange(aggCursor);
            assert.eq(change.operationType, "rename", tojson(change));
            assert.eq(change.to, {db: otherDB.getName(), coll: collOtherDB.getName()}, tojson(change));
            // Rename across databases also drops the source collection after the collection is
            // copied over.
            cst.assertNextChangesEqual({
                cursor: aggCursor,
                expectedChanges: [
                    {
                        operationType: "drop",
                        ns: {db: testDB.getName(), coll: collToInvalidate.getName()},
                    },
                ],
            });
        }

        // Test the behavior of a change stream watching the target collection of a $out
        // aggregation stage.
        collToInvalidate.aggregate([{$out: "renamed_coll"}]);
        // Do not check the 'ns' field since it will contain the namespace of the temp
        // collection created by the $out stage, before renaming to 'renamed_coll'.
        const rename = cst.getOneChange(aggCursor);
        assert.eq(rename.operationType, "rename", tojson(rename));
        assert.eq(rename.to, {db: testDB.getName(), coll: "renamed_coll"}, tojson(rename));

        // The change stream should not be invalidated by the rename(s).
        assert.eq(0, cst.getNextBatch(aggCursor).nextBatch.length);
        assert.commandWorked(collToInvalidate.insert({_id: 2}));
        assert.eq(cst.getOneChange(aggCursor).operationType, "insert");

        // Drop the "system.views" collection to avoid view catalog errors in subsequent tests.
        assertDropCollection(testDB, "system.views");

        // Recreate the test collection for the remainder of the test.
        assert.commandWorked(collToInvalidate.insert({_id: 0}));
        cst.assertNextChangesEqual({
            cursor: aggCursor,
            expectedChanges: [
                {
                    operationType: "insert",
                    ns: {db: testDB.getName(), coll: collToInvalidate.getName()},
                    documentKey: {_id: 0},
                    fullDocument: {_id: 0},
                },
            ],
        });
    }

    // Dropping a collection should generate a 'drop' entry.
    assertDropCollection(testDB, collToInvalidate.getName());
    // Insert to the test collection to queue up another change after the drop. This is needed
    // since the number of 'drop' notifications is not deterministic in the sharded passthrough
    // suites.
    assert.commandWorked(collToInvalidate.insert({_id: 0}));
    cst.consumeDropUpTo({
        cursor: aggCursor,
        dropType: "drop",
        expectedNext: {
            documentKey: {_id: 0},
            fullDocument: {_id: 0},
            ns: {db: testDB.getName(), coll: collToInvalidate.getName()},
            operationType: "insert",
        },
    });

    // Operations on internal "system" collections should be filtered out and not included in
    // the change stream.
    aggCursor = cst.startWatchingAllChangesForCluster();
    // Creating a view will generate an insert entry on the "system.views" collection.
    assert.commandWorked(testDB.runCommand({create: "view1", viewOn: collToInvalidate.getName(), pipeline: []}));
    // Drop the "system.views" collection.
    assertDropCollection(testDB, "system.views");
    // Verify that the change stream does not report the insertion into "system.views", and is
    // not invalidated by dropping the system collection. Instead, it correctly reports the next
    // write to the test collection.
    assert.commandWorked(collToInvalidate.insert({_id: 1}));
    change = cst.getOneChange(aggCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.ns, {db: testDB.getName(), coll: collToInvalidate.getName()});
}

cst.cleanUp();
