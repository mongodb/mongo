/*
 * Auth test for the bulkWrite command on mongods.
 * @tags: [
 * requires_fcv_80
 * ]
 */
import {runTest} from "jstests/auth/lib/bulk_write_base.js";

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);
