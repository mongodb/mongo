// Try to create two identical indexes, via background. Shouldn't be allowed by the server.
(function() {
var t = db.duplIndexTest;
t.drop();
docs = [];
for (var i = 0; i < 10000; i++) {
    docs.push({name: "foo", z: {a: 17, b: 4}, i: i});
}
assert.commandWorked(t.insert(docs));
var cmd = "assert.commandWorked(db.duplIndexTest.ensureIndex( { i : 1 }, {background:true} ));";
var join1 = startParallelShell(cmd);
var join2 = startParallelShell(cmd);
assert.commandWorked(t.ensureIndex({i: 1}, {background: true}));
assert.eq(1, t.find({i: 1}).count(), "Should find only one doc");
assert.commandWorked(t.dropIndex({i: 1}));
assert.eq(1, t.find({i: 1}).count(), "Should find only one doc");
join1();
join2();
})();
