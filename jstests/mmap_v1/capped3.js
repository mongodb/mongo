t = db.jstests_capped3;
t2 = db.jstests_capped3_clone;
t.drop();
t2.drop();
for (i = 0; i < 1000; ++i) {
    t.save({i: i});
}
assert.commandWorked(db.runCommand({
    cloneCollectionAsCapped: "jstests_capped3",
    toCollection: "jstests_capped3_clone",
    size: 100000
}),
                     "A");
c = t2.find();
for (i = 0; i < 1000; ++i) {
    assert.eq(i, c.next().i, "B");
}
assert(!c.hasNext(), "C");

t.drop();
t2.drop();

for (i = 0; i < 1000; ++i) {
    t.save({i: i});
}
assert.commandWorked(db.runCommand({
    cloneCollectionAsCapped: "jstests_capped3",
    toCollection: "jstests_capped3_clone",
    size: 1000
}),
                     "D");
c = t2.find().sort({$natural: -1});
i = 999;
while (c.hasNext()) {
    assert.eq(i--, c.next().i, "E");
}
// print( "i: " + i );
var str = tojson(t2.stats());
// print( "stats: " + tojson( t2.stats() ) );
assert(i < 990, "F");

t.drop();
t2.drop();

for (i = 0; i < 1000; ++i) {
    t.save({i: i});
}
assert.commandWorked(t.convertToCapped(1000), "G");
c = t.find().sort({$natural: -1});
i = 999;
while (c.hasNext()) {
    assert.eq(i--, c.next().i, "H");
}
assert(i < 990, "I");
assert(i > 900, "J");
