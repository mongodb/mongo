import {runAllRoleManagementCommandsTests} from "jstests/auth/role_management_commands_lib.js";

var conn = MongoRunner.runMongod({auth: '', useHostname: false});
runAllRoleManagementCommandsTests(conn);
MongoRunner.stopMongod(conn);
