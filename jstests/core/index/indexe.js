// @tags: [requires_getmore, requires_fastcount]

let t = db.indexe;
t.drop();

let num = 1000;

for (let i = 0; i < num; i++) {
    t.insert({a: "b"});
}

assert.eq(num, t.find().count(), "A1");
assert.eq(num, t.find({a: "b"}).count(), "B1");
assert.eq(num, t.find({a: "b"}).itcount(), "C1");

t.createIndex({a: 1});

assert.eq(num, t.find().count(), "A2");
assert.eq(num, t.find().sort({a: 1}).count(), "A2a");
assert.eq(num, t.find({a: "b"}).count(), "B2");
assert.eq(num, t.find({a: "b"}).itcount(), "C3");

t.drop();
