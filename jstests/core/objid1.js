t = db.objid1;
t.drop();

b = new ObjectId();
assert(b.str, "A");

a = new ObjectId(b.str);
assert.eq(a.str, b.str, "B");

t.save({a: a});
assert(t.findOne().a.isObjectId, "C");
assert.eq(a.str, t.findOne().a.str, "D");

x = {
    a: new ObjectId()
};
eval(" y = " + tojson(x));
assert.eq(x.a.str, y.a.str, "E");
