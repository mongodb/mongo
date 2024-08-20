/*
 * Auth test for the $listLocalSessions aggregation stage on sharded clusters.
 * @tags: [requires_sharding]
 */
import {runListLocalSessionsTest} from "jstests/auth/list_local_sessions_base.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st =
    new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
runListLocalSessionsTest(st.s0);
st.stop();