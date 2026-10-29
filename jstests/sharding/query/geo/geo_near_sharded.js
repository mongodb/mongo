// SERVER-7906
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let coll = "points";

function test(st, db, sharded, indexType) {
    printjson(db);

    if (sharded) {
        let shards = [st.shard0, st.shard1, st.shard2];

        assert.commandWorked(
            st.s0.adminCommand({shardCollection: db[coll].getFullName(), key: {rand: 1}}),
        );
        // Split at 0.1, 0.2, ... 0.9 first, then distribute the narrow chunks. Splitting before
        // moving keeps every migration a whole-chunk move: no shard ever donates a wide chunk and
        // later receives a narrower overlapping sub-range, which would be rejected because the
        // donated range stays reachable by point-in-time reads on its former owner.
        for (let i = 1; i < 10; i++) {
            assert.commandWorked(
                st.s0.adminCommand({split: db[coll].getFullName(), middle: {rand: i / 10}}),
            );
        }
        for (let i = 1; i < 10; i++) {
            assert.commandWorked(
                st.s0.adminCommand({
                    moveChunk: db[coll].getFullName(),
                    find: {rand: i / 10},
                    to: shards[i % shards.length].shardName,
                }),
            );
        }

        let config = db.getSiblingDB("config");
        assert.eq(findChunksUtil.countChunksForNs(config, db[coll].getFullName()), 10);
    }

    Random.setRandomSeed();

    let bulk = db[coll].initializeUnorderedBulkOp();
    let numPts = 10 * 1000;
    for (let i = 0; i < numPts; i++) {
        let lat = 90 - Random.rand() * 180;
        let lng = 180 - Random.rand() * 360;
        bulk.insert({rand: Math.random(), loc: [lng, lat]});
    }
    assert.commandWorked(bulk.execute());
    assert.eq(db[coll].count(), numPts);

    assert.commandWorked(db[coll].createIndex({loc: indexType}));

    let res = assert.commandWorked(
        db.runCommand({
            aggregate: coll,
            cursor: {},
            pipeline: [
                {
                    $geoNear: {
                        near: [0, 0],
                        spherical: true,
                        includeLocs: "match",
                        distanceField: "dist",
                    },
                },
            ],
        }),
        tojson({sharded: sharded, indexType: indexType}),
    );
    assert.gt(res.cursor.firstBatch.length, 0, tojson(res));
}

let st = new ShardingTest({shards: 3, mongos: 1});
assert.commandWorked(
    st.s0.adminCommand({enablesharding: "test", primaryShard: st.shard1.shardName}),
);

test(st, st.getDB("test"), true, "2dsphere");
st.stop();
