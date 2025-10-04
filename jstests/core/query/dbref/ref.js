db.otherthings.drop();
db.things.drop();

let other = {s: "other thing", n: 1};
db.otherthings.save(other);

// Verify that the DBPointer prototype is not serializable
assert.throws(function () {
    db.things.save({a: DBPointer.prototype});
});

db.things.save({name: "abc"});
let x = db.things.findOne();
x.o = new DBPointer("otherthings", other._id);
db.things.save(x);

assert(db.things.findOne().o.fetch().n == 1, "dbref broken 2");

other.n++;
db.otherthings.save(other);
assert(db.things.findOne().o.fetch().n == 2, "dbrefs broken");

db.getSiblingDB("otherdb").dropDatabase();
let objid = new ObjectId();
db.getSiblingDB("otherdb").getCollection("othercoll").insert({_id: objid, field: "value"});
let subdoc = db.getSiblingDB("otherdb").getCollection("othercoll").findOne({_id: objid});

db.mycoll.drop();
db.mycoll.insert({_id: "asdf", asdf: new DBRef("othercoll", objid, "otherdb")});
let doc = db.mycoll.findOne({_id: "asdf"}, {_id: 0, asdf: 1});
assert.eq(tojson(doc.asdf.fetch()), tojson(subdoc), "otherdb dbref");
