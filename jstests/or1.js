t = db.jstests_or1;
t.drop();

// assuming saved in order, hopefully ok just for this test

doTest = function() {

t.save( {_id:0,a:1} );
t.save( {_id:1,a:2} );
t.save( {_id:2,b:1} );
t.save( {_id:3,b:2} );
t.save( {_id:4,a:1,b:1} );
t.save( {_id:5,a:1,b:2} );
t.save( {_id:6,a:2,b:1} );
t.save( {_id:7,a:2,b:2} );

assert.throws( function() { t.find( { $or:"a" } ).toArray(); } );
assert.throws( function() { t.find( { $or:[] } ).toArray(); } );
assert.throws( function() { t.find( { $or:[ "a" ] } ).toArray(); } );

a1 = t.find( { $or: [ { a : 1 } ] } ).toArray();
assert.eq( [ { _id:0, a:1 }, { _id:4, a:1, b:1 }, { _id:5, a:1, b:2 } ], a1 );

a1b2 = t.find( { $or: [ { a : 1 }, { b : 2 } ] } ).toArray();
assert.eq( [ { _id:0, a:1 }, { _id:3, b:2 }, { _id:4, a:1, b:1 }, { _id:5, a:1, b:2 }, { _id:7, a:2, b:2 } ], a1b2 );

t.drop();
t.save( {a:[0,1],b:[0,1]} );
assert.eq( 1, t.find( { $or: [ { a: {$in:[0,1]}}  ] } ).toArray().length );
assert.eq( 1, t.find( { $or: [ { b: {$in:[0,1]}}  ] } ).toArray().length );
assert.eq( 1, t.find( { $or: [ { a: {$in:[0,1]}}, { b: {$in:[0,1]}}  ] } ).toArray().length );

}

doTest();

// not part of SERVER-1003, but good check for subseq. implementations
t.drop();
t.ensureIndex( {a:1} );
doTest();

t.drop();
t.ensureIndex( {b:1} );
doTest();

t.drop();
t.ensureIndex( {a:1,b:1} );
doTest();