// @tags: [requires_sharding]

import {runAllUserManagementCommandsTests} from "jstests/auth/user_management_commands_lib.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({shards: 2, config: 3, keyFile: 'jstests/libs/key1'});
runAllUserManagementCommandsTests(st.s, {w: 'majority', wtimeout: 60 * 1000});
st.stop();