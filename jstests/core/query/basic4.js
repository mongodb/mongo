t = db.getCollection("basic4");
t.drop();

t.save({a: 1, b: 1.0});

assert(t.findOne());
assert(t.findOne({a: 1}));
assert(t.findOne({a: 1.0}));
assert(t.findOne({b: 1}));
assert(t.findOne({b: 1.0}));

assert(!t.findOne({b: 2.0}));
