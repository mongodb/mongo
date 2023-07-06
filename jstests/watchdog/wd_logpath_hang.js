// Storage Node Watchdog - validate watchdog monitors --logpath
//
import {CharybdefsControl} from "jstests/watchdog/lib/charybdefs_lib.js";
import {testFuseAndMongoD} from "jstests/watchdog/lib/wd_test_common.js";

let control = new CharybdefsControl("logpath_hang");

const logpath = control.getMountPath();

testFuseAndMongoD(control, {logpath: logpath + "/foo.log"});
