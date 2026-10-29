/**
 * Tests that the balancer disregards a collection whose "allow chunk operations" flag is false, and
 * that disabling the flag on one collection does not affect the balancing of others.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {setAllowChunkOperations} from "jstests/sharding/libs/set_allow_chunk_operations_util.js";

describe("balancer honors allowChunkOperations", function () {
    const dbName = "allowChunkOperations";
    const bigString = "X".repeat(1024 * 1024); // 1MB, larger than the 1MB chunk size below.

    let st, configDB, primaryShard, otherShard;

    before(function () {
        st = new ShardingTest({shards: 2, other: {chunkSize: 1}});
        configDB = st.s.getDB("config");
        primaryShard = st.shard0.shardName;
        otherShard = st.shard1.shardName;
    });

    after(function () {
        st.stop();
    });

    beforeEach(function () {
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard}));
    });

    afterEach(function () {
        st.stopBalancer();
        assert.commandWorked(st.s.getDB(dbName).dropDatabase());
    });

    // Shards `coll` on {_id: 1} and splits it into 4 chunks, all left on the primary shard so the
    // collection starts out imbalanced.
    function setUpImbalancedCollection(coll) {
        assert.commandWorked(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
        );
        for (const id of [1, 10, 20, 30]) {
            assert.commandWorked(coll.insert({_id: id, s: bigString}));
        }
    }

    function chunksOnShard(coll, shardName) {
        return findChunksUtil
            .findChunksByNs(configDB, coll.getFullName(), {shard: shardName})
            .count();
    }

    function assertImbalanced(coll) {
        const status = assert.commandWorked(
            st.s.adminCommand({balancerCollectionStatus: coll.getFullName()}),
        );
        assert.eq(status.balancerCompliant, false, status);
        assert.eq(status.firstComplianceViolation, "chunksImbalance", status);
        assert.eq(
            0,
            chunksOnShard(coll, otherShard),
            "expected all chunks to remain on the primary shard",
        );
    }

    it("does not balance a collection with allowChunkOperations=false", function () {
        const coll = st.s.getCollection(`${dbName}.testColl`);
        setUpImbalancedCollection(coll);
        assertImbalanced(coll);

        // With chunk operations disabled the balancer must leave the collection untouched.
        setAllowChunkOperations(st, coll.getFullName(), false);
        st.startBalancer();
        st.awaitBalancerRound();
        st.stopBalancer();
        assertImbalanced(coll);

        // Re-enabling the flag lets the balancer converge.
        setAllowChunkOperations(st, coll.getFullName(), true);
        st.startBalancer();
        st.awaitBalance("testColl", dbName);
        st.stopBalancer();
        st.verifyCollectionIsBalanced(coll);
    });

    it("only affects the targeted namespace", function () {
        const targetedColl = st.s.getCollection(`${dbName}.targetedColl`);
        const otherColl = st.s.getCollection(`${dbName}.otherColl`);
        setUpImbalancedCollection(targetedColl);
        setUpImbalancedCollection(otherColl);
        assertImbalanced(targetedColl);
        assertImbalanced(otherColl);

        setAllowChunkOperations(st, targetedColl.getFullName(), false);

        // The untargeted collection must still balance...
        st.startBalancer();
        st.awaitBalance("otherColl", dbName);
        st.stopBalancer();
        st.verifyCollectionIsBalanced(otherColl);

        // ...while the targeted collection keeps all of its chunks on the primary shard.
        assertImbalanced(targetedColl);
    });
});
