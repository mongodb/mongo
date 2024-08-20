/*
 * Test that the balancer redistributes data from multiple tracked collections across the
 * cluster and it is able to converge within a limited amount of time.
 * (Data amount & distribution, as well as per-collection maxChunkSize, are randomly chosen).
 *
 *  @tags: [
 *      does_not_support_stepdowns, # TODO SERVER-89797 remove this tag.
 *  ]
 * */

import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const numShards = 2;
Random.setRandomSeed();

const st = new ShardingTest({shards: numShards});

const clusterMaxChunkSizeMB = 8;
const collectionBalancedTimeoutMS = 10 * 60 * 1000 /* 10min */;

const numDatabases = numShards;
const numCollInDB = 3;
const dbNamePrefix = 'test_db_';
const collNamePrefix = 'coll_';

// 1. Setup an initial set of collections.
for (let i = 0; i < numDatabases; ++i) {
    const dbName = dbNamePrefix + `${i}`;
    const primaryShardId = st[`shard${i}`].shardName;
    // Avoid assigning the same primary shard for every collection.
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardId}));
    for (let j = 0; j < numCollInDB; ++j) {
        let collName = collNamePrefix + `${j}`;
        const coll = st.s.getDB(dbName)[collName];
        const ns = coll.getFullName();
        // Use {_id: 1} as shard key to allow room for imbalance as documents get later inserted.
        st.s.adminCommand({shardCollection: ns, key: {_id: 1}});
        const collMaxChunkSizeMB = Random.randInt(clusterMaxChunkSizeMB - 1) + 1;
        assert.commandWorked(st.s.adminCommand({
            configureCollectionBalancing: ns,
            chunkSize: collMaxChunkSizeMB,
        }));
    }
}

// 2. Launch the balancer and start multiple workers inserting random data into the existing
//    collections.
st.startBalancer();

function doBatchInserts(
    numDatabases, dbNamePrefix, numCollInDB, collNamePrefix, clusterMaxChunkSizeMB) {
    Random.setRandomSeed();
    const numOfBatchInserts = 8;
    const bigString =
        'X'.repeat(1024 * 1024 - 30);  // Almost 1MB, to create documents of exactly 1MB

    for (let i = 0; i < numOfBatchInserts; ++i) {
        const dbName = dbNamePrefix + `${Random.randInt(numDatabases)}`;
        let randomDB = db.getSiblingDB(dbName);
        const collName = collNamePrefix + `${Random.randInt(numCollInDB)}`;
        const coll = randomDB[collName];

        const numDocs = Random.randInt(clusterMaxChunkSizeMB - 1) + 1;
        let insertBulkOp = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < numDocs; ++i) {
            insertBulkOp.insert({s: bigString});
        }

        assert.commandWorked(insertBulkOp.execute());
    }
}

const numBackgroundBatchInserters = 5;
let backgroundBatchInserters = [];
for (let i = 0; i < numBackgroundBatchInserters; ++i) {
    backgroundBatchInserters.push(startParallelShell(funWithArgs(doBatchInserts,
                                                                 numDatabases,
                                                                 dbNamePrefix,
                                                                 numCollInDB,
                                                                 collNamePrefix,
                                                                 clusterMaxChunkSizeMB),
                                                     st.s.port));
}

// 3. Once the insertion workers are done, verify that the balancer may bring each tracked
//    collection to a "balanced" state within the deadline.
for (let joinInserter of backgroundBatchInserters) {
    joinInserter();
}

let testedAtLeastOneCollection = false;
for (let i = 0; i < numDatabases; i++) {
    const dbName = dbNamePrefix + `${i}`;
    for (let j = 0; j < numCollInDB; j++) {
        const ns = dbName + '.' + collNamePrefix + `${j}`;

        const coll = st.s.getCollection(ns);
        if (coll.countDocuments({}) === 0) {
            // Skip empty collections
            continue;
        }
        testedAtLeastOneCollection = true;

        // Wait for collection to be considered balanced
        sh.awaitCollectionBalance(coll, collectionBalancedTimeoutMS, 1000 /* 1s interval */);
        sh.verifyCollectionIsBalanced(coll);
    }

    assert(testedAtLeastOneCollection);
}

st.stop();
