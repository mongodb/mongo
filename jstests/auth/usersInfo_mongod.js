/*
 * Test behavior and edge cases in usersInfo on mongods.
 */

import {runTest} from "jstests/auth/usersInfo_base.js";

const m = MongoRunner.runMongod();
runTest(m);
MongoRunner.stopMongod(m);