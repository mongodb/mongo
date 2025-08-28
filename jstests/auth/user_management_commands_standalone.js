import {runAllUserManagementCommandsTests} from "jstests/auth/user_management_commands_lib.js";

let conn = MongoRunner.runMongod({auth: "", useHostname: false});
runAllUserManagementCommandsTests(conn);
MongoRunner.stopMongod(conn);
