
t = db.getCollection("basic2");
t.drop();

o = {
    n: 2
};
t.save(o);

assert.eq(1, t.find().count());

assert.eq(2, t.find(o._id).toArray()[0].n);
assert.eq(2, t.find(o._id, {n: 1}).toArray()[0].n);

t.remove(o._id);
assert.eq(0, t.find().count());

assert(t.validate().valid);
