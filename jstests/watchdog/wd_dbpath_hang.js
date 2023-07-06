// Storage Node Watchdog - validate --dbpath
//
import {CharybdefsControl} from "jstests/watchdog/lib/charybdefs_lib.js";
import {testFuseAndMongoD} from "jstests/watchdog/lib/wd_test_common.js";

let control = new CharybdefsControl("dbpath_hang");

const dbPath = control.getMountPath() + "/db";

testFuseAndMongoD(control, {dbpath: dbPath});
