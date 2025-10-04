// @tags: [requires_sharding]

import {runAllRoleManagementCommandsTests} from "jstests/auth/role_management_commands_lib.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2, config: 3, keyFile: "jstests/libs/key1", useHostname: false});
runAllRoleManagementCommandsTests(st.s, {w: 1});
st.stop();
