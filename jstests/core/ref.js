// to run: 
//   ./mongo jstests/ref.js

db.otherthings.drop();
db.things.drop();

var other = { s : "other thing", n : 1};
db.otherthings.save(other);

db.things.save( { name : "abc" } );
x = db.things.findOne();
x.o = new DBPointer( "otherthings" , other._id );
db.things.save(x);

assert( db.things.findOne().o.fetch().n == 1, "dbref broken 2" );

other.n++;
db.otherthings.save(other);
assert( db.things.findOne().o.fetch().n == 2, "dbrefs broken" );

db.getMongo().getDB("otherdb").dropDatabase();
var objid = new ObjectId();
db.getMongo().getDB("otherdb").getCollection("othercoll").insert({_id:objid, field:"value"});
var subdoc = db.getMongo().getDB("otherdb").getCollection("othercoll").findOne({_id:objid})

db.mycoll.drop();
db.mycoll.insert({_id:"asdf", asdf:new DBRef("othercoll", objid, "otherdb")});
var doc = db.mycoll.findOne({_id:"asdf"}, {_id:0, asdf:1});
assert.eq(tojson(doc.asdf.fetch()), tojson(subdoc), "otherdb dbref");
