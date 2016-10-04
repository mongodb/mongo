// Try to create two identical indexes, via background. Shouldn't be allowed by the server.
// This test runs fairly quickly but cannot be in /jstests/. So it lives in slowNightly for now.
var t = db.duplIndexTest;
t.drop();
for (var i = 0; i < 10000; i++) {
    t.insert({name: "foo", z: {a: 17, b: 4}, i: i});
}
var cmd = "db.duplIndexTest.ensureIndex( { i : 1 }, {background:true} );";
var join1 = startParallelShell(cmd);
var join2 = startParallelShell(cmd);
t.ensureIndex({i: 1}, {background: true});
assert.eq(1, t.find({i: 1}).count(), "Should find only one doc");
t.dropIndex({i: 1});
assert.eq(1, t.find({i: 1}).count(), "Should find only one doc");
join1();
join2();
