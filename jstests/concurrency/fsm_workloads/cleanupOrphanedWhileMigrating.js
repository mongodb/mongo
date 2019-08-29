'use strict';

/**
 * Performs range deletions while chunks are being moved.
 *
 * @tags: [requires_sharding, assumes_balancer_on]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/sharded_base_partitioned.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;

    $config.data.shardKey = {skey: 1};
    $config.data.shardKeyField = 'skey';

    const numChunks = 50;
    const numDocs = 100;

    // Total count of documents when initialized.
    $config.data.initialCount = numChunks * numDocs;

    function executeCommandWithRetries(fn, sleepInterval, retries, name) {
        let result = null;
        let done = false;

        while (retries > 0 && !done) {
            result = fn();

            if (result.ok) {
                print("command succeeded: " + name);
                done = true;
            } else {
                print("command failed: " + name);
                printjson(result);
                sleep(sleepInterval);
            }
        }

        return result;
    }

    // Run cleanupOrphaned on a random shard's primary node.
    $config.states.cleanupOrphans = function(db, collName, connCache) {
        const ns = db[collName].getFullName();

        // Get index of random shard.
        const shardNames = Object.keys(connCache.shards);
        const randomIndex = Math.floor(Math.random() * shardNames.length);

        const shard = connCache.shards[shardNames[randomIndex]];
        const shardPrimary = ChunkHelper.getPrimary(shard);

        let nextKey = {};
        let result = null;

        let iteration = 0;
        while (nextKey != null) {
            result = executeCommandWithRetries(() => {
                return shardPrimary.adminCommand(
                    {cleanupOrphaned: ns, startingFromKey: nextKey, secondaryThrottle: true});
            }, 100, 1000, "cleanupOrphaned");

            nextKey = result.stoppedAtKey;
            iteration++;
        }

        assert(result.ok);
    };

    // Verify that counts are stable.
    $config.states.validate = function(db, collName, connCache) {
        const ns = db[collName].getFullName();

        // Get total count from mongos. Need to specify batch count that is larger than the total
        // number of records to prevent getmore command from being issued since stepdown suites
        // ban it.
        const mongos = ChunkHelper.getRandomMongos(connCache.mongos);
        const coll = mongos.getCollection(ns);
        const totalCount = coll.find({}).batchSize($config.data.initialCount + numDocs).itcount();

        // Verify that sum equals original total.
        assert(this.initialCount === totalCount,
               "Document count doesn't match initial count: " + this.initialCount +
                   " != " + totalCount);
    };

    $config.states.init = function init(db, collName, connCache) {};

    $config.setup = function setup(db, collName, cluster) {
        const ns = db[collName].getFullName();

        for (let chunkIndex = 0; chunkIndex < numChunks; chunkIndex++) {
            let bulk = db[collName].initializeUnorderedBulkOp();

            const splitKey = chunkIndex * numDocs;
            print("splitting at: " + splitKey);

            for (let docIndex = splitKey - numDocs; docIndex < splitKey; docIndex++) {
                bulk.insert({_id: docIndex, skey: docIndex});
            }

            assertAlways.commandWorked(bulk.execute());

            if (chunkIndex > 0) {
                // Need to retry split command to avoid conflicting with moveChunks issued by the
                // balancer.
                let result = executeCommandWithRetries(() => {
                    return db.adminCommand({split: ns, middle: {skey: splitKey}});
                }, 100, 10, "split");
                assertAlways.commandWorked(result);
            }
        }
    };

    $config.transitions = {
        init: {cleanupOrphans: 1},
        cleanupOrphans: {cleanupOrphans: 0.5, validate: 0.5},
        validate: {cleanupOrphans: 1}
    };

    return $config;
});