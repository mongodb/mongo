
a = db.ref4a;
b = db.ref4b;

a.drop();
b.drop();

db.otherthings.drop();
db.things.drop();

var other = { s : "other thing", n : 17 };
b.save(other);

a.save( { name : "abc" , others : [ new DBRef( "ref4b" , other._id ) , new DBPointer( "ref4b" , other._id ) ] } );
assert( a.findOne().others[0].fetch().n == 17 , "dbref broken 1" );

x = Array.fetchRefs( a.findOne().others );
assert.eq( 2 , x.length , "A" );
assert.eq( 17 , x[0].n , "B" );
assert.eq( 17 , x[1].n , "C" );


assert.eq( 0 , Array.fetchRefs( a.findOne().others , "z" ).length , "D" );
