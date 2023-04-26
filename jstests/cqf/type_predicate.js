(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_type_predicate;
t.drop();

for (let i = 0; i < 10; i++) {
    assert.commandWorked(t.insert({a: i}));
    assert.commandWorked(t.insert({a: i.toString()}));
}

{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$type: "string"}}}]);
    assert.eq(10, res.executionStats.nReturned);
}
{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$type: "double"}}}]);
    assert.eq(10, res.executionStats.nReturned);
}
}());
