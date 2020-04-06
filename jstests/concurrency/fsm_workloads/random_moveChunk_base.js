'use strict';

/**
 * Shards a collection by 'skey' and creates one chunk per thread, filling each chunk with
 * documents, and assigning each document to a random thread. Meant to be extended by workloads that
 * test operations with concurrent moveChunks. Assumes each thread has an id from [0, threadCount).
 *
 * @tags: [requires_sharding, assumes_balancer_off, assumes_autosplit_off];
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/sharded_base_partitioned.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.threadCount = 1;
    $config.iterations = 1;

    $config.data.shardKey = {skey: 1};
    $config.data.shardKeyField = {};

    // A 'default' field needs to be specified because the 'setup' phase of the workload
    // won't have access to each collection's current specified shard key.
    $config.data.defaultShardKeyField = 'skey';

    // Which skey and _id values are owned by this thread (they are equal by default) for each
    // collection, populated in init().
    $config.data.ownedIds = {};

    // Depending on the operations performed by each workload, it might be expected that a random
    // moveChunk may fail with an error code other than those expected by the helper.
    $config.data.isMoveChunkErrorAcceptable = (err) => false;

    /**
     * Returns the _id of a random document owned by this thread for a given collection.
     */
    $config.data.getIdForThread = function getIdForThread(collName) {
        assertAlways.neq(0, this.ownedIds[collName].size);

        const randomIndex = Random.randInt(this.ownedIds[collName].length);
        return this.ownedIds[collName][randomIndex];
    };

    /**
     * Calculates the bounds object for a shard key given a chunk object. Assumes that any
     * secondary shard key fields exist as a result of the refineCollectionShardKey command, and
     * thus fills in those fields with the corresponding MinKey/MaxKey values.
     */
    $config.data.calculateChunkBoundsForShardKey = function calculateChunkBoundsForShardKey(
        collName, chunk) {
        const shardKeyField = this.shardKeyField[collName];
        let minBound = {};
        let maxBound = {};
        if (Array.isArray(shardKeyField)) {
            minBound[shardKeyField[0]] = chunk.min[shardKeyField[0]];
            maxBound[shardKeyField[0]] = chunk.max[shardKeyField[0]];

            for (let i = 1; i < shardKeyField.length; i++) {
                minBound[shardKeyField[i]] = MinKey;
                maxBound[shardKeyField[i]] = this.partition.isHighChunk ? MaxKey : MinKey;
            }
        } else {
            minBound[shardKeyField] = chunk.min[shardKeyField];
            maxBound[shardKeyField] = chunk.max[shardKeyField];
        }

        return [minBound, maxBound];
    };

    /**
     * Picks a random chunk and moves it to a random new shard. The migration is retried on
     * acceptable errors, e.g. ConflictingOperationInProgress, and is not guaranteed to succeed.
     */
    $config.states.moveChunk = function moveChunk(db, collName, connCache) {
        // Committing a chunk migration requires acquiring the global X lock on the CSRS primary.
        // This state function is unsafe to automatically run inside a multi-statement transaction
        // because it'll have left an idle transaction on the CSRS primary before attempting to run
        // the moveChunk command, which can lead to a hang.
        fsm.forceRunningOutsideTransaction(this);

        // Choose a random chunk in our partition to move.
        const chunk = this.getRandomChunkInPartition(collName, connCache.rsConns.config);
        const fromShard = chunk.shard;

        // Choose a random shard to move the chunk to.
        const shardNames = Object.keys(connCache.shards);
        const destinationShards = shardNames.filter(function(shard) {
            if (shard !== fromShard) {
                return shard;
            }
        });
        const toShard = destinationShards[Random.randInt(destinationShards.length)];

        // Use chunk_helper.js's moveChunk wrapper to tolerate acceptable failures and to use a
        // limited number of retries with exponential backoff.
        const bounds = this.calculateChunkBoundsForShardKey(collName, chunk);
        const waitForDelete = Random.rand() < 0.5;
        try {
            ChunkHelper.moveChunk(db, collName, bounds, toShard, waitForDelete);
        } catch (e) {
            // Failed moveChunks are thrown by the moveChunk helper with the response included as a
            // JSON string in the error's message.
            if (this.isMoveChunkErrorAcceptable(e)) {
                print("Ignoring acceptable moveChunk error: " + tojson(e));
                return;
            }

            throw e;
        }
    };

    /**
     * 1. Loads this thread's partition into memory.
     * 2. Loads the _ids of owned documents into memory.
     * 3. Populates the shard key for this collection into memory.
     */
    $config.states.init = function init(db, collName, connCache) {
        // Load this thread's partition.
        const ns = db[collName].getFullName();
        this.partition = this.makePartition(ns, this.tid, this.partitionSize);

        // Create entry for this collection in ownedIds
        this.ownedIds[collName] = [];

        // Search the collection to find the _ids of docs assigned to this thread.
        const docsOwnedByThread = db[collName].find({tid: this.tid}).toArray();
        assert.neq(0, docsOwnedByThread.size);
        docsOwnedByThread.forEach(doc => {
            this.ownedIds[collName].push(doc._id);
        });

        this.shardKeyField[collName] = this.defaultShardKeyField;
    };

    /**
     * Sets up the collection so each thread's partition is a single chunk, with partitionSize
     * documents within it, randomly assigning each document to a thread, ensuring at least one
     * document is given to each one.
     */
    $config.setup = function setup(db, collName, cluster) {
        const ns = db[collName].getFullName();

        for (let tid = 0; tid < this.threadCount; ++tid) {
            // Find the thread's partition.
            const partition = this.makePartition(ns, tid, this.partitionSize);
            let bulk = db[collName].initializeUnorderedBulkOp();

            let choseThisThread = false;
            for (let i = partition.lower; i < partition.upper; ++i) {
                // Randomly assign threads, but ensure each thread is given at least one document.
                let chosenThread = Random.randInt(this.threadCount);

                choseThisThread = choseThisThread || chosenThread === tid;
                // If this is the last document being inserted into the partition, and no document
                // from this partition has yet been assigned to the current thread, assign this
                // document to the current thread.
                if (i === partition.upper - 1 && !choseThisThread) {
                    chosenThread = tid;
                }

                // Give each document the same shard key and _id value, but a different tid.
                bulk.insert({_id: i, [this.defaultShardKeyField]: i, tid: chosenThread});
            }
            assertAlways.commandWorked(bulk.execute());

            // Create a chunk with boundaries matching the partition's. The low chunk's lower bound
            // is minKey, so a split is not necessary.
            if (!partition.isLowChunk) {
                assertAlways.commandWorked(db.adminCommand(
                    {split: ns, middle: {[this.defaultShardKeyField]: partition.lower}}));
            }
        }
    };

    $config.transitions = {init: {moveChunk: 1}, moveChunk: {moveChunk: 1}};

    return $config;
});
