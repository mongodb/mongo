// to run: 
//   ./mongo jstests/ref.js

db.otherthings.drop();
db.things.drop();

var other = { s : "other thing", n : 1};
db.otherthings.save(other);

db.things.save( { name : "abc" } );
x = db.things.findOne();
x.o = other;
db.things.save(x);

assert( db.things.findOne().o.n == 1, "dbref broken 2" );

other.n++;
db.otherthings.save(other);
//print( tojson( db.things.findOne() ) );
print("ref.js: needs line uncommented after fixing bug:");
//assert( db.things.findOne().o.n == 2, "dbrefs broken" );
