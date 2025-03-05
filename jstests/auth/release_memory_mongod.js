/*
 * Run release memory auth test on a standalone mongod.
 * TODO SERVER-97456 - add sharded test
 * @tags: [
 *   requires_fcv_81
 * ]
 */
import {runTest} from "jstests/auth/release_memory_base.js";

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);
