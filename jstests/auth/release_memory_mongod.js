/*
 * Run release memory auth test on a standalone mongod.
 * @tags: [
 *   requires_fcv_81
 * ]
 */
import {runTest} from "jstests/auth/release_memory_base.js";

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);
