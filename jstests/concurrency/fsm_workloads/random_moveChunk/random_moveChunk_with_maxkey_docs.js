/**
 * Tests that documents with MaxKey() shard key values survive concurrent chunk migrations.
 * Regression test for SERVER-121533 where documents with MaxKey() field values could become
 * inaccessible after chunk migration due to bugs in extendRangeBound (incorrect MinKey padding
 * for trailing fields when the bound contains MaxKey) and index scan boundary handling (exclusive
 * upper bound missing documents whose shard key is exactly MaxKey).
 *
 * Uses a compound shard key {skey: 1, otherKey: 1} to exercise the extendRangeBound code path.
 * Inserts three patterns of MaxKey documents:
 *   - {skey: MaxKey(), otherKey: MaxKey()} -- global max
 *   - {skey: MaxKey(), otherKey: <number>} -- MaxKey on prefix only
 *   - {skey: <number>, otherKey: MaxKey()} -- MaxKey on non-prefix field
 *
 * States: insertMaxKeyDoc, updateMaxKeyDoc, moveChunk, verifyMaxKeyDocs.
 * No deletes, enabling exact count verification in teardown.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 * ]
 */
import {fsm} from "jstests/concurrency/fsm_libs/fsm.js";
import {ShardingTopologyHelpers} from "jstests/concurrency/fsm_workload_helpers/catalog_and_routing/sharding_topology_helpers.js";
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/chunks.js";
import {isMoveChunkErrorAcceptableWithConcurrent} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/move_chunk_errors.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

export const $config = (function () {
    const kNumInitialRegularDocs = 100;
    const kNumInitialMaxKeyDocsPerPattern = 4;
    const kNumMaxKeyPatterns = 3;
    const kNumInitialMaxKeyDocs = kNumInitialMaxKeyDocsPerPattern * kNumMaxKeyPatterns;
    const kCounterCollSuffix = "_maxkey_counter";

    const data = {
        shardKey: {skey: 1, otherKey: 1},
        trackedDocs: [],
    };

    data.generateMaxKeyDoc = function generateMaxKeyDoc(tid) {
        const pattern = Random.randInt(kNumMaxKeyPatterns);
        const doc = {
            _id: new ObjectId(),
            tid: tid,
            counter: 0,
        };
        switch (pattern) {
            case 0:
                doc.skey = MaxKey();
                doc.otherKey = MaxKey();
                break;
            case 1:
                doc.skey = MaxKey();
                doc.otherKey = Random.randInt(1000);
                break;
            case 2:
                doc.skey = Random.randInt(1000);
                doc.otherKey = MaxKey();
                break;
        }
        return doc;
    };

    const states = {};

    states.init = function init(db, collName, connCache) {
        this.trackedDocs = [];
    };

    states.insertMaxKeyDoc = function insertMaxKeyDoc(db, collName, connCache) {
        const doc = this.generateMaxKeyDoc(this.tid);
        assert.commandWorked(db[collName].insert(doc));
        this.trackedDocs.push({
            _id: doc._id,
            skey: doc.skey,
            otherKey: doc.otherKey,
            counter: 0,
        });
        assert.commandWorked(db[collName + kCounterCollSuffix].update({_id: "maxKeyCount"}, {$inc: {count: 1}}));
    };

    states.updateMaxKeyDoc = function updateMaxKeyDoc(db, collName, connCache) {
        if (this.trackedDocs.length === 0) {
            return;
        }
        const idx = Random.randInt(this.trackedDocs.length);
        const tracked = this.trackedDocs[idx];
        const res = db[collName].update({_id: tracked._id}, {$inc: {counter: 1}});
        if (res.nModified === 1) {
            tracked.counter++;
        }
    };

    states.moveChunk = function moveChunk(db, collName, connCache) {
        fsm.forceRunningOutsideTransaction(this);

        const configDB = connCache.rsConns.config.getDB("config");
        const ns = db[collName].getFullName();
        const chunksJoinClause = findChunksUtil.getChunksJoinClause(configDB, ns);

        const chunks = configDB
            .getCollection("chunks")
            .aggregate([{$match: chunksJoinClause}, {$sample: {size: 1}}])
            .toArray();
        if (chunks.length === 0) {
            return;
        }

        const chunk = chunks[0];
        const fromShard = chunk.shard;

        const shardNames = ShardingTopologyHelpers.getShardNames(db);
        const destinationShards = shardNames.filter((s) => s !== fromShard);
        if (destinationShards.length === 0) {
            return;
        }

        const toShard = destinationShards[Random.randInt(destinationShards.length)];
        const waitForDelete = Random.rand() < 0.5;

        try {
            ChunkHelper.moveChunk(db, collName, [chunk.min, chunk.max], toShard, waitForDelete);
        } catch (e) {
            if (isMoveChunkErrorAcceptableWithConcurrent([], e)) {
                jsTest.log.info("Ignoring acceptable moveChunk error: " + tojson(e));
                return;
            }
            throw e;
        }
    };

    states.verifyMaxKeyDocs = function verifyMaxKeyDocs(db, collName, connCache) {
        for (const tracked of this.trackedDocs) {
            const doc = db[collName].findOne({_id: tracked._id});
            assert.neq(
                doc,
                null,
                "MaxKey doc with _id " +
                    tojson(tracked._id) +
                    " was lost during migration. Expected: " +
                    tojson(tracked),
            );
            assert.eq(tojson(doc.skey), tojson(tracked.skey), "skey mismatch for doc " + tojson(doc));
            assert.eq(tojson(doc.otherKey), tojson(tracked.otherKey), "otherKey mismatch for doc " + tojson(doc));
            assert.eq(
                doc.counter,
                tracked.counter,
                "counter mismatch for doc " + tojson(doc) + ", expected counter: " + tracked.counter,
            );
        }
    };

    const transitions = {
        init: {insertMaxKeyDoc: 0.3, updateMaxKeyDoc: 0.15, moveChunk: 0.2, verifyMaxKeyDocs: 0.35},
        insertMaxKeyDoc: {insertMaxKeyDoc: 0.15, updateMaxKeyDoc: 0.2, moveChunk: 0.25, verifyMaxKeyDocs: 0.4},
        updateMaxKeyDoc: {insertMaxKeyDoc: 0.15, updateMaxKeyDoc: 0.15, moveChunk: 0.25, verifyMaxKeyDocs: 0.45},
        moveChunk: {insertMaxKeyDoc: 0.2, updateMaxKeyDoc: 0.15, moveChunk: 0.15, verifyMaxKeyDocs: 0.5},
        verifyMaxKeyDocs: {insertMaxKeyDoc: 0.2, updateMaxKeyDoc: 0.2, moveChunk: 0.35, verifyMaxKeyDocs: 0.25},
    };

    function setup(db, collName, cluster) {
        const ns = db[collName].getFullName();

        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < kNumInitialRegularDocs; i++) {
            bulk.insert({_id: i, skey: i, otherKey: i, tid: i % 5, counter: 0});
        }
        assert.commandWorked(bulk.execute());

        for (let pattern = 0; pattern < kNumMaxKeyPatterns; pattern++) {
            for (let i = 0; i < kNumInitialMaxKeyDocsPerPattern; i++) {
                const doc = {
                    _id: "maxkey_" + pattern + "_" + i,
                    tid: i % 5,
                    counter: 0,
                };
                switch (pattern) {
                    case 0:
                        doc.skey = MaxKey();
                        doc.otherKey = MaxKey();
                        break;
                    case 1:
                        doc.skey = MaxKey();
                        doc.otherKey = i;
                        break;
                    case 2:
                        doc.skey = i;
                        doc.otherKey = MaxKey();
                        break;
                }
                assert.commandWorked(db[collName].insert(doc));
            }
        }

        assert.commandWorked(
            db[collName + kCounterCollSuffix].insert({_id: "maxKeyCount", count: kNumInitialMaxKeyDocs}),
        );

        const splitPoints = [25, 50, 75];
        for (const point of splitPoints) {
            assert.commandWorked(db.adminCommand({split: ns, middle: {skey: point, otherKey: MinKey()}}));
        }

        const shardNames = Object.keys(cluster.getSerializedCluster().shards);
        if (shardNames.length >= 2) {
            const configDB = db.getSiblingDB("config");
            const chunks = findChunksUtil.findChunksByNs(configDB, ns).sort({min: 1}).toArray();
            for (let i = 0; i < chunks.length; i++) {
                const targetShard = shardNames[i % shardNames.length];
                if (chunks[i].shard !== targetShard) {
                    assert.commandWorked(
                        db.adminCommand({
                            moveChunk: ns,
                            find: chunks[i].min,
                            to: targetShard,
                            _waitForDelete: true,
                        }),
                    );
                }
            }
        }
    }

    function teardown(db, collName, cluster) {
        const counterDoc = db[collName + kCounterCollSuffix].findOne({_id: "maxKeyCount"});
        assert.neq(counterDoc, null, "MaxKey counter document not found");
        const expectedCount = counterDoc.count;

        const actualCount = db[collName].find({$or: [{skey: MaxKey()}, {otherKey: MaxKey()}]}).itcount();

        assert.eq(
            expectedCount,
            actualCount,
            "MaxKey doc count mismatch: expected " +
                expectedCount +
                " but found " +
                actualCount +
                ". Some MaxKey docs were lost during chunk migrations.",
        );
    }

    return {
        threadCount: 5,
        iterations: 50,
        startState: "init",
        states: states,
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
        passConnectionCache: true,
    };
})();
