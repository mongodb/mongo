
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

// Check for an error message when we index and there are dups 
db.bar.drop();
db.bar.insert({a:3});
db.bar.insert({a:3});
assert( db.bar.count() == 2 , 6) ;
db.bar.ensureIndex({a:1}, true);
assert( db.getLastError() , 7);

/* Check that if we update and remove _id, it gets added back by the DB */

/* - test when object grows */
t.drop();
t.save( { _id : 'Z' } );
t.update( {}, { k : 2 } );
assert( t.findOne()._id == 'Z', "uniqueness.js problem with adding back _id" );

/* - test when doesn't grow */
t.drop();
t.save( { _id : 'Z', k : 3 } );
t.update( {}, { k : 2 } );
assert( t.findOne()._id == 'Z', "uniqueness.js problem with adding back _id (2)" );

