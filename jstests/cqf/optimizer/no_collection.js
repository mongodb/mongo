import {checkCascadesOptimizerEnabled} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

let t = db.cqf_no_collection;
t.drop();

const res = t.explain("executionStats").aggregate([{$match: {'a': 2}}]);
assert.eq(0, res.executionStats.nReturned);
