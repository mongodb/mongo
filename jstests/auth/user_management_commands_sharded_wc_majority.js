(function() {
    'use strict';

    load('jstests/auth/user_management_commands_lib.js');

    var st = new ShardingTest({shards: 2, config: 3, keyFile: 'jstests/libs/key1'});
    runAllUserManagementCommandsTests(st.s, {w: 'majority', wtimeout: 60 * 1000});
    st.stop();
})();
