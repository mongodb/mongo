
t = db.foo;

t.drop();

// test uniqueness of _id

t.save( { _id : 3 } );
assert( !db.getLastError(), 1 );

// this should yield an error
t.insert( { _id : 3 } );
assert( db.getLastError() , 2);
assert( t.count() == 1, "hmmm");

t.insert( { _id : 4, x : 99 } );
assert( !db.getLastError() , 3);

// this should yield an error
t.update( { _id : 4 } , { _id : 3, x : 99 } );
assert( db.getLastError() , 4);
assert( t.findOne( {_id:4} ), 5 );

/* Check for an error message when we index and there are dups */
db.bar.drop();
db.bar.insert({a:3});
db.bar.insert({a:3});
assert( db.bar.count() == 2 , 6) ;
db.bar.ensureIndex({a:1}, true);
assert( db.getLastError() , 7);
