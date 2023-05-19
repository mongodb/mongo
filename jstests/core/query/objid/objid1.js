let t = db.objid1;
t.drop();

let b = new ObjectId();
assert(b.str, "A");

let a = new ObjectId(b.str);
assert.eq(a.str, b.str, "B");

t.save({a: a});
assert(t.findOne().a.isObjectId, "C");
assert.eq(a.str, t.findOne().a.str, "D");

let x = {a: new ObjectId()};
eval(" y = " + tojson(x));
assert.eq(x.a.str, y.a.str, "E");
