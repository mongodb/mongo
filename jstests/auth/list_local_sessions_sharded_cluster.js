/*
 * Auth test for the $listLocalSessions aggregation stage on sharded clusters.
 * @tags: [requires_sharding]
 */
(function() {
'use strict';

load("jstests/auth/list_local_sessions_base.js");

const st =
    new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
runListLocalSessionsTest(st.s0);
st.stop();
})();
