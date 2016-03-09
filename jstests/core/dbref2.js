
a = db.dbref2a;
b = db.dbref2b;
c = db.dbref2c;

a.drop();
b.drop();
c.drop();

a.save({name: "eliot"});
b.save({num: 1, link: new DBRef("dbref2a", a.findOne()._id)});
c.save({num: 1, links: [new DBRef("dbref2a", a.findOne()._id)]});

assert.eq("eliot", b.findOne().link.fetch().name, "A");
assert.neq("el", b.findOne().link.fetch().name, "B");

// $elemMatch value
var doc = c.findOne({links: {$elemMatch: {$ref: "dbref2a", $id: a.findOne()._id}}});
assert.eq("eliot", doc.links[0].fetch().name, "C");
assert.neq("el", doc.links[0].fetch().name, "D");
