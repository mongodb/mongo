import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 0});

// System session collection can't be sharded when there are no shards in the cluster
assert.commandFailed(st.s.adminCommand({shardCollection: "config.system.sessions", key: {_id: 1}}));

st.stop();
