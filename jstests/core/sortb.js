// Test that the in memory sort capacity limit is checked for all "top N" sort candidates.
// SERVER-4716

t = db.jstests_sortb;
t.drop();

t.ensureIndex({b: 1});

for (i = 0; i < 100; ++i) {
    t.save({a: i, b: i});
}

// These large documents will not be part of the initial set of "top 100" matches, and they will
// not be part of the final set of "top 100" matches returned to the client.  However, they are an
// intermediate set of "top 100" matches and should trigger an in memory sort capacity exception.
big = new Array(1024 * 1024).toString();
for (i = 100; i < 200; ++i) {
    t.save({a: i, b: i, big: big});
}

for (i = 200; i < 300; ++i) {
    t.save({a: i, b: i});
}

assert.throws(function() {
    t.find().sort({a: -1}).hint({b: 1}).limit(100).itcount();
});
assert.throws(function() {
    t.find().sort({a: -1}).hint({b: 1}).showDiskLoc().limit(100).itcount();
});
t.drop();