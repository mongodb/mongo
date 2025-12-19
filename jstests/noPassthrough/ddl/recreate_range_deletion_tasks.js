/**
 * Test that `recreateRangeDeletionTasks` correctly flags unowned ranges as orphaned on all shards knowing a collection.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

Random.setRandomSeed();

const st = new ShardingTest({
    shards: {shard0: {nodes: 1}, shard1: {nodes: 1}, shard2: {nodes: 1}},
    rs: {setParameter: {disableResumableRangeDeleter: true}},
    config: 1,
});

const mongos = st.s;
const shards = [st.shard0, st.shard1, st.shard2];

const dbName = "test";
const hashedShardKeyCollName = "collWithHashedShardKey";
const simpleShardKeyCollName = "collWithSimpleShardKey";
const compoundHashedShardKeyCollName = "collWithCompoundHashedShardKey";
const compoundShardKeyCollName = "collWithCompoundShardKey";

const hashedShardKeyNs = dbName + "." + hashedShardKeyCollName;
const simpleShardKeyNs = dbName + "." + simpleShardKeyCollName;
const compoundHashedShardKeyNs = dbName + "." + compoundHashedShardKeyCollName;
const compoundShardKeyNs = dbName + "." + compoundShardKeyCollName;

const testDb = mongos.getDB(dbName);

function checkNoRangeDeletions(fullNs) {
    const [dbName, collName] = fullNs.split(".");
    const chunks = findChunksUtil.findChunksByNs(mongos.getDB("config"), fullNs);
    chunks.forEach((chunk) => {
        shards.forEach((shard) => {
            const nRangeDelDocs = shard.getDB("config").rangeDeletions.countDocuments({
                nss: fullNs,
                collectionUuid: chunk.uuid,
            });
            assert.eq(
                0,
                nRangeDelDocs,
                "Found unexpected range deletion docs on " + shard.shardName + " for collection " + fullNs,
            );
        });
    });
}

function checkRangeDeletionsRecreated(fullNs, shardKey) {
    const [dbName, collName] = fullNs.split(".");
    const chunks = findChunksUtil.findChunksByNs(mongos.getDB("config"), fullNs);
    chunks.forEach((chunk) => {
        shards.forEach((shard) => {
            const rangeDelDoc = shard.getDB("config").rangeDeletions.findOne({
                nss: fullNs,
                collectionUuid: chunk.uuid,
                keyPattern: shardKey,
                pending: {$ne: true} /* recreated tasks must never be pending */,
                // Match recreated tasks per orphaned range (may span multiple chunks)
                "range.min": {"$lte": chunk.min},
                "range.max": {"$gte": chunk.max},
            });

            const collExistsOnShard =
                assert.commandWorked(shard.getDB(dbName).runCommand({listCollections: 1, filter: {name: collName}}))
                    .cursor.firstBatch.length == 1;

            if (!collExistsOnShard || shard.shardName == chunk.shard) {
                // If the shard has no local incarnation of the collection or the shard is the actual owner for the chunk, the range deletion task must not have been recreated.
                assert.eq(
                    undefined,
                    rangeDelDoc,
                    "Unexpectedly found range deletion document on " +
                        shard.shardName +
                        " for " +
                        JSON.stringify(chunk),
                );
            } else {
                assert(rangeDelDoc, "Range deletion not found on " + shard.shardName + " for " + JSON.stringify(chunk));
            }
        });
    });
}

function shardCollectionAndRecreateRangeDeletions(fullNs, shardKey, skipEmptyRanges) {
    const [dbName, collName] = fullNs.split(".");
    assert.commandWorked(testDb.adminCommand({shardCollection: fullNs, key: shardKey}));

    // Move to random shard the chunk with bounds [0, 100) for all shard key fields
    const randomShard = shards[Random.randInt(shards.length)].shardName;
    const min = Object.fromEntries(Object.keys(shardKey).map((field) => [field, 0]));
    const max = Object.fromEntries(Object.keys(shardKey).map((field) => [field, 100]));
    assert.commandWorked(mongos.adminCommand({moveRange: fullNs, min: min, max: max, toShard: randomShard}));

    assert.commandWorked(testDb.runCommand({recreateRangeDeletionTasks: collName, skipEmptyRanges: skipEmptyRanges}));
    checkRangeDeletionsRecreated(fullNs, shardKey);
}

const skipEmptyRanges = false;
// Sharding a collection with hashed-prefixed shard key initially distributes the shard key space over all the shards
shardCollectionAndRecreateRangeDeletions(hashedShardKeyNs, {h: "hashed"}, skipEmptyRanges);
shardCollectionAndRecreateRangeDeletions(compoundHashedShardKeyNs, {h: "hashed", sk0: 1, sk1: 1}, skipEmptyRanges);

// Sharding a collection with non-hashed-prefixed shard key initially creates one chunk on the primary shard (`shardCollectionAndRecreateRangeDeletions` randomly moves around some data)
shardCollectionAndRecreateRangeDeletions(simpleShardKeyNs, {sk: 1}, skipEmptyRanges);
shardCollectionAndRecreateRangeDeletions(compoundShardKeyNs, {sk0: 1, sk1: 1}, skipEmptyRanges);

function testSkipEmptyRanges() {
    // Distribute an empty collection with hashed shard key over all shards
    const initiallyEmptyCollName = "initiallyEmptyColl";
    const emptyNs = dbName + "." + initiallyEmptyCollName;
    const shardKey = {h: "hashed"};
    assert.commandWorked(testDb.adminCommand({shardCollection: emptyNs, key: shardKey}));

    // Invoking the command must not create any range deletion on any shard because the whole collection is empty
    assert.commandWorked(
        testDb.runCommand({recreateRangeDeletionTasks: initiallyEmptyCollName, skipEmptyRanges: true}),
    );
    checkNoRangeDeletions(emptyNs);

    // Insert a bunch of documents directly on shards to create orphaned docs in every non-owned range
    shards.forEach((shard) => {
        const docs = Array.from({length: 100}, (_, i) => ({h: i}));
        assert.commandWorked(shard.getDB(dbName).runCommand({insert: initiallyEmptyCollName, documents: docs}));
    });

    // Reinvoking the command must create range deletions for every non-owned range because they're not empty
    assert.commandWorked(
        testDb.runCommand({recreateRangeDeletionTasks: initiallyEmptyCollName, skipEmptyRanges: false}),
    );
    checkRangeDeletionsRecreated(emptyNs, shardKey);
}

testSkipEmptyRanges();

// Re-enable the range deleter to double check that new tasks are valid and eventually drain during teardown
shards.forEach((shard) => {
    assert.commandWorked(shard.adminCommand({setParameter: 1, disableResumableRangeDeleter: false}));
});

st.stop();
