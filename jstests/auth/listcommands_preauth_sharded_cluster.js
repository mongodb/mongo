/*
 * Make sure that listCommands on sharded clusters doesn't require authentication.
 * @tags: [requires_sharding]
 */
(function() {
'use strict';

load("jstests/auth/listcommands_preauth_base.js");

const st =
    new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
runTest(st.s0);
st.stop();
})();
