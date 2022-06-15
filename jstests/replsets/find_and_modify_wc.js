//
// Tests writeConcerns with findAndModify command
//
(function() {
'use strict';

// Skip this test when running with storage engines other than inMemory, as the test relies on
// journaling not being active.
if (jsTest.options().storageEngine !== "inMemory") {
    jsTest.log("Skipping test because it is only applicable for the inMemory storage engine");
    return;
}

var nodeCount = 3;
var rst = new ReplSetTest({nodes: nodeCount});
rst.startSet();
rst.initiate();

var primary = rst.getPrimary();
var coll = primary.getCollection("test.find_and_modify_wc");
coll.remove({});

// insert some documents
var docs = [];
for (var i = 1; i <= 5; ++i) {
    docs.push({i: i, j: 2 * i});
}
var res = coll.runCommand({insert: coll.getName(), documents: docs, writeConcern: {w: nodeCount}});
assert(res.ok);
assert.eq(5, coll.find().itcount());

// use for updates in subsequent runCommand calls
var reqUpdate = {
    findAndModify: coll.getName(),
    query: {i: 3},
    update: {$inc: {j: 1}},
    writeConcern: {w: 'majority'}
};

// Verify findAndModify returns old document new: false
var res = coll.runCommand(reqUpdate);
assert(res.ok);
assert(res.value);
// (2 * res.value.i) == 6 == res.value.j (old document)
assert.eq(2 * res.value.i, res.value.j);
assert(!res.writeConcernError);

// Verify findAndModify returns new document with new: true
reqUpdate.new = true;
res = coll.runCommand(reqUpdate);
assert(res.ok);
assert(res.value);
// (2 * res.value.i + 2) == 8 == res.value.j (new document after two updates)
assert.eq(2 * res.value.i + 2, res.value.j);
assert(!res.writeConcernError);

// Verify findAndModify remove works
res = coll.runCommand(
    {findAndModify: coll.getName(), sort: {i: 1}, remove: true, writeConcern: {w: nodeCount}});
assert.eq(res.value.i, 1);
assert.eq(coll.find().itcount(), 4);
assert(!res.writeConcernError);

// Verify findAndModify returns writeConcernError
// when given invalid writeConcerns
[{w: 'invalid'}, {w: nodeCount + 1}].forEach(function(wc) {
    reqUpdate.writeConcern = wc;
    res = coll.runCommand(reqUpdate);

    assert(res.writeConcernError);
    assert(res.writeConcernError.code);
    assert(res.writeConcernError.errmsg);
});

rst.stopSet();
})();
