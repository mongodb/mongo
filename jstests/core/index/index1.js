// @tags: [requires_non_retryable_writes]

let t = db.embeddedIndexTest;

t.remove({});

let o = {name: "foo", z: {a: 17, b: 4}};
t.save(o);

assert(t.findOne().z.a == 17);
assert(t.findOne({z: {a: 17}}) == null);

t.createIndex({"z.a": 1});

assert(t.findOne().z.a == 17);
assert(t.findOne({z: {a: 17}}) == null);

o = {
    name: "bar",
    z: {a: 18},
};
t.save(o);

assert.eq(2, t.find().length());
assert.eq(2, t.find().sort({"z.a": 1}).length());
assert.eq(2, t.find().sort({"z.a": -1}).length());

assert(t.validate().valid);
