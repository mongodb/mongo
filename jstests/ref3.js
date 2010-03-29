// to run: 
//   ./mongo jstests/ref3.js

db.otherthings3.drop();
db.things3.drop();

var other = { s : "other thing", n : 1};
db.otherthings3.save(other);

db.things3.save( { name : "abc" } );
x = db.things3.findOne();
x.o = new DBRef( "otherthings3" , other._id );
db.things3.save(x);

assert( db.things3.findOne().o.fetch().n == 1, "dbref broken 2" );

other.n++;
db.otherthings3.save(other);
assert( db.things3.findOne().o.fetch().n == 2, "dbrefs broken" );
