var res;
const t = db[jsTestName()];
t.drop();

function doTest() {
    assert.commandWorked(t.save({a: [{id: 1, x: [5, 6, 7]}, {id: 2, x: [8, 9, 10]}]}));
    assert.commandWorked(t.update({'a.id': 1}, {$set: {'a.$.x': [1, 1, 1]}}));
    assert.eq(1, t.findOne().a[0].x[0]);
}

doTest();
assert(t.drop());
assert.commandWorked(t.createIndex({'a.id': 1}));
doTest();
