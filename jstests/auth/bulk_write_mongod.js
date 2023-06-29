/*
 * Auth test for the bulkWrite command on mongods.
 */
import {runTest} from "jstests/auth/lib/bulk_write_base.js";

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);
