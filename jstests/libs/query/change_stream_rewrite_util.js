/**
 * Helper functions that are used in change streams rewrite test cases.
 */

import {
    assertCreateCollection,
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// Function which generates a write workload on the specified collection, including all events that
// a change stream may consume. Assumes that the specified collection does not already exist.
export function generateChangeStreamWriteWorkload(
    db, collName, numDocs, includInvalidatingEvents = true) {
    // If this is a sharded passthrough, make sure we shard on something other than _id so that a
    // non-id field appears in the documentKey. This will generate 'create' and 'shardCollection'.
    if (FixtureHelpers.isMongos(db)) {
        assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
        assert.commandWorked(db.adminCommand(
            {shardCollection: `${db.getName()}.${collName}`, key: {shardKey: "hashed"}}));
    }

    // If the collection hasn't already been created, do so here.
    let testColl = assertCreateCollection(db, collName);

    // Build an index, collMod it, then drop it.
    assert.commandWorked(testColl.createIndex({a: 1}));
    assert.commandWorked(db.runCommand({
        collMod: testColl.getName(),
        index: {keyPattern: {a: 1}, hidden: true, expireAfterSeconds: 500}
    }));
    assert.commandWorked(testColl.dropIndex({a: 1}));

    // Modify the collection's validation options.
    assert.commandWorked(testColl.runCommand({
        collMod: collName,
        validator: {},
        validationLevel: "off",
        validationAction: "warn",
    }));

    // Change the validation options back.
    assert.commandWorked(testColl.runCommand({
        collMod: collName,
        validator: {},
        validationLevel: "strict",
        validationAction: "error",
    }));

    // Insert some documents.
    for (let i = 0; i < numDocs; ++i) {
        assert.commandWorked(testColl.insert(
            {_id: i, shardKey: i, a: [1, [2], {b: 3}], f1: {subField: true}, f2: false}));
    }

    // Update half of them. We generate these updates individually so that they generate different
    // values for the 'updatedFields', 'removedFields' and 'truncatedArrays' subfields.
    const updateSpecs = [
        [{$set: {f2: true}}],                                // only populates 'updatedFields'
        [{$unset: ["f1"]}],                                  // only populates 'removedFields'
        [{$set: {a: [1, [2]]}}],                             // only populates 'truncatedArrays'
        [{$set: {a: [1, [2]], f2: true}}, {$unset: ["f1"]}]  // populates all fields
    ];
    for (let i = 0; i < numDocs / 2; ++i) {
        assert.commandWorked(
            testColl.update({_id: i, shardKey: i}, updateSpecs[(i % updateSpecs.length)]));
    }

    // Replace the other half.
    for (let i = numDocs / 2; i < numDocs; ++i) {
        assert.commandWorked(testColl.replaceOne({_id: i, shardKey: i}, {_id: i, shardKey: i}));
    }

    // Delete half of the updated documents.
    for (let i = 0; i < numDocs / 4; ++i) {
        assert.commandWorked(testColl.remove({_id: i, shardKey: i}));
    }

    // Create, modify, and drop a view on the collection.
    assert.commandWorked(db.createView("view", collName, []));
    assert.commandWorked(db.runCommand({collMod: "view", viewOn: "viewOnView", pipeline: []}));
    assertDropCollection(db, "view");

    // If the caller is prepared to handle potential invalidations, include the following events.
    if (includInvalidatingEvents) {
        // Rename the collection.
        const collNameAfterRename = `${testColl.getName()}_renamed`;
        assert.commandWorked(testColl.renameCollection(collNameAfterRename));
        testColl = db[collNameAfterRename];

        // Rename it back.
        assert.commandWorked(testColl.renameCollection(collName));
        testColl = db[collName];

        // Drop the collection.
        assert(testColl.drop());

        // Drop the database.
        assert.commandWorked(db.dropDatabase());
    }
    return testColl;
}

// Helper function to fully exhaust a change stream from the specified point and return all events.
// Assumes that all relevant events can fit into a single 16MB batch.
export function getAllChangeStreamEvents(
    db, extraPipelineStages = [], csOptions = {}, resumeToken) {
    // Open a whole-cluster stream based on the supplied arguments.
    const csCursor = db.getMongo().watch(
        extraPipelineStages,
        Object.assign({resumeAfter: resumeToken, maxAwaitTimeMS: 1}, csOptions));

    // Run getMore until the post-batch resume token advances. In a sharded passthrough, this will
    // guarantee that all shards have returned results, and we expect all results to fit into a
    // single batch, so we know we have exhausted the stream.
    while (bsonWoCompare(csCursor._postBatchResumeToken, resumeToken) == 0) {
        csCursor.hasNext();  // runs a getMore
    }

    // Close the cursor since we have already retrieved all results.
    csCursor.close();

    // Extract all events from the streams. Since the cursor is closed, it will not attempt to
    // retrieve any more batches from the server.
    return csCursor.toArray();
}

// Helper function to check whether this value is a plain old javascript object.
export function isPlainObject(value) {
    return (value && typeof (value) == "object" && value.constructor === Object);
}

// Verifies the number of change streams events returned from a particular shard.
export function assertNumChangeStreamDocsReturnedFromShard(
    stats, shardName, expectedTotalReturned) {
    assert(stats.shards.hasOwnProperty(shardName), stats);
    const stages = stats.shards[shardName].stages;
    const lastStage = stages[stages.length - 1];
    assert.eq(lastStage.nReturned, expectedTotalReturned, stages);
}

export function getExecutionStatsForShard(stats, shardName) {
    assert(stats.shards.hasOwnProperty(shardName), stats);
    assert.eq(Object.keys(stats.shards[shardName].stages[0])[0], "$cursor", stats);
    return stats.shards[shardName].stages[0].$cursor.executionStats;
}

export function getUnwindStageForShard(stats, shardName) {
    assert(stats.shards.hasOwnProperty(shardName), stats);
    const stages = stats.shards[shardName].stages;
    assert.eq(stages[1].$changeStream.stage, "internalUnwindTransaction");
    return stages[1];
}

// Verifies the number of oplog events read by a particular shard.
export function assertNumMatchingOplogEventsForShard(stats, shardName, expectedTotalReturned) {
    const executionStats = getExecutionStatsForShard(stats, shardName);
    // We verify the number of documents from the unwind stage instead of the oplog cursor, so we
    // are testing that the filter is applied to the output of batched oplog entries as well.
    const unwindStage = getUnwindStageForShard(stats, shardName);
    assert.eq(unwindStage.nReturned,
              expectedTotalReturned,
              () => `Expected ${expectedTotalReturned} events on shard ${shardName} but got ` +
                  `${executionStats.nReturned}. Execution stats:\n${tojson(executionStats)}\n` +
                  `Unwind stage:\n${tojson(unwindStage)}`);
}

// Returns a newly created sharded collection sharded by caller provided shard key.
export function createShardedCollection(shardingTest, shardKey, dbName, collName, splitAt) {
    assert.commandWorked(shardingTest.s.adminCommand(
        {enableSharding: dbName, primaryShard: shardingTest.shard0.name}));

    const db = shardingTest.s.getDB(dbName);
    assertDropAndRecreateCollection(db, collName);

    const coll = db.getCollection(collName);
    assert.commandWorked(coll.createIndex({[shardKey]: 1}));

    // Shard the test collection and split it into two chunks: one that contains all {shardKey: <lt
    // splitAt>} documents and one that contains all {shardKey: <gte splitAt>} documents.
    shardingTest.shardColl(
        collName,
        {[shardKey]: 1} /* shard key */,
        {[shardKey]: splitAt} /* split at */,
        {[shardKey]: splitAt} /* move the chunk containing {shardKey: splitAt} to its own shard */,
        dbName,
        true);
    return coll;
}

// A helper that opens a change stream on the whole cluster with the user supplied match expression
// 'userMatchExpr' and 'changeStreamSpec'. The helper validates that:
// 1. for each shard, the events are seen in the order specified by 'expectedResult' which structure
//    is { collOrDbName : { operationType : [eventIdentifier1, eventIdentifier2, ..], ... }, .. }.
// 2. There are no additional events being returned other than the ones in the 'expectedResult'.
// 3. the filtering is been done at oplog level, and each of the shard read only the
// 'expectedOplogNReturnedPerShard' documents.
export function verifyChangeStreamOnWholeCluster({
    st,
    changeStreamSpec,
    userMatchExpr,
    expectedResult,
    expectedOplogNReturnedPerShard,
    expectedChangeStreamDocsReturnedPerShard
}) {
    changeStreamSpec["allChangesForCluster"] = true;
    const adminDB = st.s.getDB("admin");
    const cursor = adminDB.aggregate([{$changeStream: changeStreamSpec}, userMatchExpr]);

    for (const [collOrDb, opDict] of Object.entries(expectedResult)) {
        for (const [op, eventIdentifierList] of Object.entries(opDict)) {
            eventIdentifierList.forEach(eventIdentifier => {
                assert.soon(() => cursor.hasNext(), {op: op, eventIdentifier: eventIdentifier});
                const event = cursor.next();
                assert.eq(event.operationType,
                          op,
                          () => `Expected "${op}" but got "${event.operationType}". Full event: ` +
                              `${tojson(event)}`);

                if (op == "dropDatabase") {
                    assert.eq(event.ns.db, eventIdentifier, event);
                } else if (op == "insert" || op == "update" || op == "replace" || op == "delete") {
                    assert.eq(event.documentKey._id, eventIdentifier, event);
                } else if (op == "rename") {
                    assert.eq(event.to.coll, eventIdentifier, event);
                } else if (op == "drop") {
                    assert.eq(event.ns.coll, eventIdentifier);
                } else if (op == "create") {
                    assert.eq(event.ns.coll, eventIdentifier);
                } else if (op == "createIndexes") {
                    assert.eq(event.ns.coll, eventIdentifier);
                } else if (op == "dropIndexes") {
                    assert.eq(event.ns.coll, eventIdentifier);
                } else if (op == "shardCollection") {
                    assert.eq(event.ns.coll, eventIdentifier);
                } else if (op == "modify") {
                    assert.eq(event.ns.coll, eventIdentifier);
                } else {
                    assert(false, event);
                }

                if (op != "dropDatabase") {
                    assert.eq(event.ns.coll, collOrDb);
                }
            });
        }
    }

    assert(!cursor.hasNext(), () => tojson(cursor.next()));

    const stats = adminDB.runCommand({
        explain: {
            aggregate: 1,
            pipeline: [{$changeStream: changeStreamSpec}, userMatchExpr],
            cursor: {batchSize: 0}
        },
        verbosity: "executionStats"
    });

    assertNumMatchingOplogEventsForShard(
        stats, st.shard0.shardName, expectedOplogNReturnedPerShard[0]);
    assertNumMatchingOplogEventsForShard(
        stats, st.shard1.shardName, expectedOplogNReturnedPerShard[1]);

    if (expectedChangeStreamDocsReturnedPerShard !== undefined) {
        assertNumChangeStreamDocsReturnedFromShard(
            stats, st.shard0.shardName, expectedChangeStreamDocsReturnedPerShard[0]);
        assertNumChangeStreamDocsReturnedFromShard(
            stats, st.shard1.shardName, expectedChangeStreamDocsReturnedPerShard[1]);
    }
}
