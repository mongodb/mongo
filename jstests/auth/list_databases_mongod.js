/*
 * Auth test for the listDatabases command on mongods.
 */
import {runTest} from "jstests/auth/list_databases_base.js";

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);