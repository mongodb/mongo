/*
 * Make sure that listCommands on mongods doesn't require authentication.
 */
import {runTest} from "jstests/auth/listcommands_preauth_base.js";

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);