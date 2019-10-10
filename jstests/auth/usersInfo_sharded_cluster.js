/*
 * Test behavior and edge cases in usersInfo on sharded clusters.
 * @tags: [requires_sharding]
 */
(function() {
'use strict';

load("jstests/auth/usersInfo_base.js");

const st = new ShardingTest({shards: 1, mongos: 1, config: 1});
runTest(st.s0);
st.stop();
}());
