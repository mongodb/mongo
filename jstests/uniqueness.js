
t = db.jstests_uniqueness;

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
db.jstests_uniqueness2.drop();
db.jstests_uniqueness2.insert({a:3});
db.jstests_uniqueness2.insert({a:3});
assert( db.jstests_uniqueness2.count() == 2 , 6) ;
db.resetError();
db.jstests_uniqueness2.ensureIndex({a:1}, true);
assert( db.getLastError() , 7);
assert( db.getLastError().match( /E11000/ ) );

// Check for an error message when we index in the background and there are dups 
db.jstests_uniqueness2.drop();
db.jstests_uniqueness2.insert({a:3});
db.jstests_uniqueness2.insert({a:3});
assert( db.jstests_uniqueness2.count() == 2 , 6) ;
assert( !db.getLastError() );
db.resetError();
db.jstests_uniqueness2.ensureIndex({a:1}, {unique:true,background:true});
assert( db.getLastError() , 7);
assert( db.getLastError().match( /E11000/ ) );

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

