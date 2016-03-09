/* test indexing where the key is an embedded object.
 */

t = db.embeddedIndexTest2;

t.drop();
assert(t.findOne() == null);

o = {
    name: "foo",
    z: {a: 17}
};
p = {
    name: "foo",
    z: {a: 17}
};
q = {
    name: "barrr",
    z: {a: 18}
};
r = {
    name: "barrr",
    z: {k: "zzz", L: [1, 2]}
};

t.save(o);

assert(t.findOne().z.a == 17);

t.save(p);
t.save(q);

assert(t.findOne({z: {a: 17}}).z.a == 17);
assert(t.find({z: {a: 17}}).length() == 2);
assert(t.find({z: {a: 18}}).length() == 1);

t.save(r);

assert(t.findOne({z: {a: 17}}).z.a == 17);
assert(t.find({z: {a: 17}}).length() == 2);
assert(t.find({z: {a: 18}}).length() == 1);

t.ensureIndex({z: 1});

assert(t.findOne({z: {a: 17}}).z.a == 17);
assert(t.find({z: {a: 17}}).length() == 2);
assert(t.find({z: {a: 18}}).length() == 1);

assert(t.find().sort({z: 1}).length() == 4);
assert(t.find().sort({z: -1}).length() == 4);

assert(t.validate().valid);
