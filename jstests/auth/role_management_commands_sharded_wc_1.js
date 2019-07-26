// @tags: [requires_sharding]

(function() {
'use strict';

load('jstests/auth/role_management_commands_lib.js');

// TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
var st = new ShardingTest({
    shards: 2,
    config: 3,
    keyFile: 'jstests/libs/key1',
    useHostname: false,
    other: {shardAsReplicaSet: false}
});
runAllRoleManagementCommandsTests(st.s, {w: 1});
st.stop();
})();
