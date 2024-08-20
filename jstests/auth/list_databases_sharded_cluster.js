/*
 * Auth test for the listDatabases command on sharded clusters.
 * @tags: [requires_sharding]
 */
import {runTest} from "jstests/auth/list_databases_base.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st =
    new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
runTest(st.s0);
st.stop();