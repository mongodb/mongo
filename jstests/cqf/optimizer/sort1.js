(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_sort1;
t.drop();

t.insert({a: [{b: 1}, {c: 1}], d: {e: 1, f: 1}});
t.insert({a: [{b: 1, c: 1}], d: {e: 1, f: 1}});
t.insert({a: {b: 1, c: 1}, d: [{e: 1, f: 1}]});
t.insert({a: {b: 1, c: 1}, d: [{e: 1}, {f: 1}]});

const index = {
    'a.b': 1,
    'a.c': 1,
    'd.e': 1,
    'd.f': 1
};
t.createIndex(index);

{
    // We should not be satisfying more than one common multikey requirement per candidate index. In
    // the query below, the paths 'a.b' and 'a.c' share a multikey component ("a"). Thus, they
    // cannot both be satisfied with the same index scan. The paths 'a.b' and 'd.e' however can be
    // satisfied in the same index scan since they do not share a multikey component.
    const query = {'a.b': 1, 'a.c': 1, 'd.e': 1, 'd.f': 1};

    const resCollScan = t.find(query).hint({$natural: 1}).sort({_id: 1}).toArray();
    const resIndexScan = t.find(query).hint(index).sort({_id: 1}).toArray();
    assert.eq(resCollScan, resIndexScan);
}
}());
