
t = db.maxscan;
t.drop();

N = 100;
for (i = 0; i < N; i++) {
    t.insert({_id: i, x: i % 10});
}

assert.eq(N, t.find().itcount(), "A");
assert.eq(50, t.find().maxScan(50).itcount(), "B");

assert.eq(10, t.find({x: 2}).itcount(), "C");
assert.eq(5, t.find({x: 2}).maxScan(50).itcount(), "D");

t.ensureIndex({x: 1});
assert.eq(10, t.find({x: 2}).hint({x: 1}).maxScan(N).itcount(), "E");
assert.eq(0, t.find({x: 2}).hint({x: 1}).maxScan(1).itcount(), "E");
