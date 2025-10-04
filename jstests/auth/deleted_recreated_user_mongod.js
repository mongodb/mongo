/*
 * Test that sessions on mongods cannot be resumed by deleted and recreated user.
 */
import {runTest} from "jstests/auth/deleted_recreated_user_base.js";

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod, mongod);
MongoRunner.stopMongod(mongod);
