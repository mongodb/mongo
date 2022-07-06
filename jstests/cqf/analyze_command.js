(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const coll = db.cqf_analyze;
coll.drop();

assert.commandWorked(coll.insert({a: [1, 2, 4, 4, 5, 6]}));

let res = db.runCommand({analyze: coll.getName()});
assert.commandFailedWithCode(res, ErrorCodes.NotImplemented);

res = db.runCommand({analyze: coll.getName(), apiVersion: "1", apiStrict: true});
assert.commandFailedWithCode(res, ErrorCodes.APIStrictError);

res = db.runCommand({analyze: coll.getName(), writeConcern: {w: 1}});
assert.commandFailedWithCode(res, ErrorCodes.NotImplemented);
}());
