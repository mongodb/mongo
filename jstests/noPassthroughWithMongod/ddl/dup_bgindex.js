// Try to create two identical indexes, via background. Shouldn't be allowed by the server.
let t = db.duplIndexTest;
t.drop();
let docs = [];
for (let i = 0; i < 10000; i++) {
    docs.push({name: "foo", z: {a: 17, b: 4}, i: i});
}
assert.commandWorked(t.insert(docs));
let cmd = "assert.commandWorked(db.duplIndexTest.createIndex( { i : 1 } ));";
let join1 = startParallelShell(cmd);
let join2 = startParallelShell(cmd);
assert.commandWorked(t.createIndex({i: 1}));
assert.eq(1, t.find({i: 1}).count(), "Should find only one doc");
assert.commandWorked(t.dropIndex({i: 1}));
assert.eq(1, t.find({i: 1}).count(), "Should find only one doc");
join1();
join2();
