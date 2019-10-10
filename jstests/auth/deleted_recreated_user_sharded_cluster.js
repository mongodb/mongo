/*
 * Test that sessions on sharded clusters cannot be resumed by deleted and recreated user.
 * @tags: [requires_sharding]
 */
(function() {
'use strict';

load("jstests/auth/deleted_recreated_user_base.js");

const st = new ShardingTest({
    shards: 1,
    mongos: 2,
    config: 1,
    other: {
        keyFile: 'jstests/libs/key1',
        mongosOptions: {
            setParameter: 'userCacheInvalidationIntervalSecs=' + kInvalidationIntervalSecs,
        },
    },
});
runTest(st.s0, st.s1);
st.stop();
})();
