t = db.objid3;
t.drop();

t.save({a: "bob", _id: 517});
for (var k in t.findOne()) {
    assert.eq(k, "_id", "keys out of order");
    break;
}
