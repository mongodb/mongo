
a = db.dbref1a;
b = db.dbref1b;

a.drop();
b.drop();

a.save({name: "eliot"});
b.save({num: 1, link: new DBPointer("dbref1a", a.findOne()._id)});
assert.eq("eliot", b.findOne().link.fetch().name, "A");
