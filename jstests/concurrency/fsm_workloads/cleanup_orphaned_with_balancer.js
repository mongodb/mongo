/**
 * Performs range deletions while chunks are being moved.
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_on,
 *   antithesis_incompatible,
 *   does_not_support_stepdowns,
 * ]
 */

import {BalancerHelper} from "jstests/concurrency/fsm_workload_helpers/balancer.js";
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/chunks.js";

export const $config = (function () {
    let data = {
        shardKey: {skey: 1},
        shardKeyField: "skey",
        initialCount: 5000,
    };

    let states = {
        // Run cleanupOrphaned on a random shard's primary node.
        cleanupOrphans: function (db, collName, connCache) {
            const ns = db[collName].getFullName();

            // Get index of random shard.
            const shardNames = Object.keys(connCache.shards);
            const randomIndex = Math.floor(Math.random() * shardNames.length);

            const shardConn = connCache.rsConns.shards[shardNames[randomIndex]];

            // Disable balancing so that waiting for orphan cleanup can converge quickly.
            BalancerHelper.disableBalancerForCollection(db, ns);

            // Ensure the cleanup of all chunk orphans of the primary shard
            assert.soonNoExcept(
                () => {
                    assert.commandWorked(shardConn.adminCommand({cleanupOrphaned: ns}));
                    return true;
                },
                undefined,
                10 * 1000,
                100,
            );

            // Reenable balancing.
            BalancerHelper.enableBalancerForCollection(db, ns);
        },

        // Verify that counts are stable.
        validate: function (db, collName, connCache) {
            const ns = db[collName].getFullName();

            // Get total count from mongos. Need to specify batch count that is larger than the
            // total number of records to prevent getmore command from being issued since
            // stepdown suites ban it.
            const mongos = ChunkHelper.getRandomMongos(connCache.mongos);
            const coll = mongos.getCollection(ns);
            const totalCount = coll
                .find({})
                .batchSize(data.initialCount * 2)
                .itcount();

            // Verify that sum equals original total.
            assert(
                this.initialCount === totalCount,
                "Document count doesn't match initial count: " + this.initialCount + " != " + totalCount,
            );
        },

        init: function init(db, collName, connCache) {},
    };

    let setup = function (db, collName, cluster) {
        let bulk = db[collName].initializeUnorderedBulkOp();
        for (let index = 0; index < data.initialCount; index++) {
            bulk.insert({_id: index, skey: index});
        }
        assert.commandWorked(bulk.execute());
    };

    let transitions = {
        init: {cleanupOrphans: 1},
        cleanupOrphans: {cleanupOrphans: 0.5, validate: 0.5},
        validate: {cleanupOrphans: 1},
    };

    return {
        threadCount: 5,
        iterations: 50,
        startState: "init",
        data: data,
        states: states,
        transitions: transitions,
        setup: setup,
        passConnectionCache: true,
    };
})();
