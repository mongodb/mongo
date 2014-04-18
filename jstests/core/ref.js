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
