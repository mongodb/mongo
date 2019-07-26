/**
 * @tags: [requires_sharding]
 */

(function() {
'use strict';

load('jstests/auth/user_management_commands_lib.js');

// TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
var st = new ShardingTest(
    {shards: 2, config: 3, keyFile: 'jstests/libs/key1', other: {shardAsReplicaSet: false}});
runAllUserManagementCommandsTests(st.s, {w: 1});
st.stop();
})();
