/**
 * Tests that the operation deadline is correctly carried forward and refreshed
 * during getMore commands.
 *
 * Uses the $assertDeadlineIncreaseAfterBatch extension stage which verifies that:
 *   - Within a batch, the deadline does not change between getNext() calls.
 *   - At the batch boundary (when a getMore is issued), the deadline increases.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const kExtensionLib = "libassert_deadline_increased_after_batch_extension.so";

const NUM_DOCS = 10;

function setupCollection(conn, db, coll) {
    if (conn.isMongos()) {
        // If we’re on mongos, set up sharding for this collection. Distribute documents across the two shards to ensure we don't target a single shard during execution.
        const res = db.adminCommand({listShards: 1});
        assert.commandWorked(res);
        const shardIds = res.shards.map((s) => s._id);
        assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
        assert.commandWorked(coll.createIndex({x: 1}));
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {x: Math.floor(NUM_DOCS / 2)}}));

        assert(shardIds.length > 1);
        assert.commandWorked(db.adminCommand({moveChunk: coll.getFullName(), find: {x: 2}, to: shardIds[0]}));
        assert.commandWorked(db.adminCommand({moveChunk: coll.getFullName(), find: {x: 7}, to: shardIds[1]}));
    }

    const bulkDocs = [];
    for (let i = 0; i < NUM_DOCS; i++) {
        bulkDocs.push({_id: i, x: i});
    }
    coll.insertMany(bulkDocs);
}

function runDeadlineTests(conn, _shardingTest) {
    const db = conn.getDB("test");
    const collName = jsTestName();
    const coll = db[collName];
    coll.drop();
    setupCollection(conn, db, coll);

    const sleepTimeBetweenGetMore = 50;
    const maxTimeMs = 5 * 1000;

    // Test 1: With a batchSize of 3 and maxTimeMS (5 seconds), iterating through multiple getMore
    // batches should succeed because the deadline is refreshed at each batch boundary.
    (function testDeadlineRefreshedAcrossGetMores() {
        jsTest.log("Test: deadline is refreshed across getMore batches");
        const batchSize = 3;

        const result = db.runCommand({
            aggregate: collName,
            pipeline: [{$sort: {x: 1}}, {$assertDeadlineIncreaseAfterBatch: {batchSize: batchSize}}],
            cursor: {batchSize: batchSize},
            maxTimeMS: maxTimeMs,
        });
        assert.commandWorked(result);
        let cursorId = result.cursor.id;

        // The initial batch should have returned 'batchSize' documents.
        assert.eq(result.cursor.firstBatch.length, batchSize);
        // Sanity check, sleep for maxTimeMs, query succeeds since deadline is updated each getMore.
        sleep(maxTimeMs);
        // Iterate through the remaining documents via getMore.
        let totalDocs = result.cursor.firstBatch.length;
        while (cursorId != 0) {
            // Sleep to give the ensure the deadline always moves forward.
            sleep(sleepTimeBetweenGetMore);
            const getMoreResult = assert.commandWorked(
                db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}),
            );

            totalDocs += getMoreResult.cursor.nextBatch.length;
            cursorId = getMoreResult.cursor.id;
        }

        assert.eq(totalDocs, NUM_DOCS, "Expected all documents to be returned");
    })();

    // Test 2: Using the shell cursor helper with maxTimeMS and batchSize to verify
    // deadline propagation across multiple getMore operations.
    (function testDeadlineWithCursorHelper() {
        jsTest.log("Test: deadline propagation via shell cursor helper");
        const batchSize = 3;

        const cursor = coll.aggregate([{$sort: {x: 1}}, {$assertDeadlineIncreaseAfterBatch: {batchSize: batchSize}}], {
            cursor: {batchSize: batchSize},
            maxTimeMS: maxTimeMs,
        });

        // Exhaust the cursor; if the deadline is not refreshed correctly,
        // the extension stage will trigger a uassert.
        let count = 0;
        while (cursor.hasNext()) {
            sleep(sleepTimeBetweenGetMore);
            cursor.next();
            count++;
        }
        assert.eq(count, NUM_DOCS, "Expected all documents to be returned");
    })();

    // Test 3: Multiple batch boundaries with a small batchSize to ensure
    // deadline refresh happens at each getMore, not just the first.
    (function testMultipleBatchBoundaries() {
        jsTest.log("Test: multiple batch boundaries with batchSize=1");
        const batchSize = 1;

        const result = db.runCommand({
            aggregate: collName,
            pipeline: [{$sort: {x: 1}}, {$assertDeadlineIncreaseAfterBatch: {batchSize: batchSize}}],
            cursor: {batchSize: batchSize},
            maxTimeMS: maxTimeMs,
        });
        assert.commandWorked(result);
        let cursorId = result.cursor.id;

        let totalDocs = result.cursor.firstBatch.length;
        // Issue multiple getMore commands, each fetching 1 document.
        while (cursorId != 0) {
            sleep(sleepTimeBetweenGetMore);
            const getMoreResult = assert.commandWorked(
                db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}),
            );
            totalDocs += getMoreResult.cursor.nextBatch.length;
            cursorId = getMoreResult.cursor.id;
        }

        assert.eq(totalDocs, NUM_DOCS, "Expected all documents to be returned");
    })();
}

withExtensions({[kExtensionLib]: {}}, runDeadlineTests, ["sharded", "standalone"], {shards: 2});
