//
// Tests that the changelog entry for moveChunk.commit contains stats on the migration.
//

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let st = new ShardingTest({mongos: 1, shards: 2});
let kDbName = "db";

let mongos = st.s0;
let shard0 = st.shard0.shardName;
let shard1 = st.shard1.shardName;

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

function assertCountsInChangelog() {
    let changeLog = st.s.getDB("config").changelog.find({what: "moveChunk.commit"}).toArray();
    assert.gt(changeLog.length, 0);
    for (let i = 0; i < changeLog.length; i++) {
        assert(changeLog[i].details.hasOwnProperty("counts"));
    }
}

let ns = kDbName + ".fooHashed";
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: "hashed"}}));

let aChunk = findChunksUtil.findOneChunkByNs(mongos.getDB("config"), ns, {shard: shard0});
assert(aChunk);

// Assert counts field exists in the changelog entry for moveChunk.commit
assert.commandWorked(mongos.adminCommand({moveChunk: ns, bounds: [aChunk.min, aChunk.max], to: shard1}));
assertCountsInChangelog();

mongos.getDB(kDbName).fooHashed.drop();

st.stop();
