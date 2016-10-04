// test max docs in capped collection

var t = db.capped_max1;
t.drop();

var max = 10;
var maxSize = 64 * 1024;
db.createCollection(t.getName(), {capped: true, size: maxSize, max: max});
assert.eq(max, t.stats().max);
assert.eq(maxSize, t.stats().maxSize);
assert.eq(Math.floor(maxSize / 1000), t.stats(1000).maxSize);

for (var i = 0; i < max * 2; i++) {
    t.insert({x: i});
}

assert.eq(max, t.count());

// Test invalidation of cursors
var cursor = t.find().batchSize(4);
assert(cursor.hasNext());
var myX = cursor.next();
for (var j = 0; j < max * 2; j++) {
    t.insert({x: j + i});
}

// Cursor should now be dead.
assert.throws(function() {
    cursor.toArray();
});
