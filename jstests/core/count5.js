
t = db.count5;
t.drop();

for (i = 0; i < 100; i++) {
    t.save({x: i});
}

q = {
    x: {$gt: 25, $lte: 75}
};

assert.eq(50, t.find(q).count(), "A");
assert.eq(50, t.find(q).itcount(), "B");

t.ensureIndex({x: 1});

assert.eq(50, t.find(q).count(), "C");
assert.eq(50, t.find(q).itcount(), "D");

assert.eq(50, t.find(q).limit(1).count(), "E");
assert.eq(1, t.find(q).limit(1).itcount(), "F");

assert.eq(5, t.find(q).limit(5).size(), "G");
assert.eq(5, t.find(q).skip(5).limit(5).size(), "H");
assert.eq(2, t.find(q).skip(48).limit(5).size(), "I");

assert.eq(20, t.find().limit(20).size(), "J");

assert.eq(0, t.find().skip(120).size(), "K");
assert.eq(1, db.runCommand({count: "count5"})["ok"], "L");
assert.eq(1, db.runCommand({count: "count5", skip: 120})["ok"], "M");
