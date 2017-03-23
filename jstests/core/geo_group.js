t = db.geo_group;
t.drop();

n = 1;
var bulk = t.initializeUnorderedBulkOp();
for (var x = -100; x < 100; x += 2) {
    for (var y = -100; y < 100; y += 2) {
        bulk.insert({_id: n++, loc: [x, y]});
    }
}
assert.writeOK(bulk.execute());

t.ensureIndex({loc: "2d"});

// Test basic count with $near
assert.eq(t.find().count(), 10000);
assert.eq(t.find({loc: {$within: {$center: [[56, 8], 10]}}}).count(), 81);
assert.eq(t.find({loc: {$near: [56, 8, 10]}}).count(), 81);

// Test basic group that effectively does a count
assert.eq(t.group({
    reduce: function(obj, prev) {
        prev.sums = {count: prev.sums.count + 1};
    },
    initial: {sums: {count: 0}}
}),
          [{"sums": {"count": 10000}}]);

// Test basic group + $near that does a count
assert.eq(t.group({
    reduce: function(obj, prev) {
        prev.sums = {count: prev.sums.count + 1};
    },
    initial: {sums: {count: 0}},
    cond: {loc: {$near: [56, 8, 10]}}
}),
          [{"sums": {"count": 81}}]);
