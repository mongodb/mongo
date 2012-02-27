// test min / max query parameters

addData = function() {
    t.save( { a: 1, b: 1 } );
    t.save( { a: 1, b: 2 } );
    t.save( { a: 2, b: 1 } );
    t.save( { a: 2, b: 2 } );    
}

t = db.jstests_minmax;
t.drop();
t.ensureIndex( { a: 1, b: 1 } );
addData();

printjson( t.find().min( { a: 1, b: 2 } ).max( { a: 2, b: 1 } ).toArray() );
assert.eq( 1, t.find().min( { a: 1, b: 2 } ).max( { a: 2, b: 1 } ).toArray().length );
assert.eq( 2, t.find().min( { a: 1, b: 2 } ).max( { a: 2, b: 1.5 } ).toArray().length );
assert.eq( 2, t.find().min( { a: 1, b: 2 } ).max( { a: 2, b: 2 } ).toArray().length );

// just one bound
assert.eq( 3, t.find().min( { a: 1, b: 2 } ).toArray().length );
assert.eq( 3, t.find().max( { a: 2, b: 1.5 } ).toArray().length );
assert.eq( 3, t.find().min( { a: 1, b: 2 } ).hint( { a: 1, b: 1 } ).toArray().length );
assert.eq( 3, t.find().max( { a: 2, b: 1.5 } ).hint( { a: 1, b: 1 } ).toArray().length );

t.drop();
t.ensureIndex( { a: 1, b: -1 } );
addData();
assert.eq( 4, t.find().min( { a: 1, b: 2 } ).toArray().length );
assert.eq( 4, t.find().max( { a: 2, b: 0.5 } ).toArray().length );
assert.eq( 1, t.find().min( { a: 2, b: 1 } ).toArray().length );
assert.eq( 1, t.find().max( { a: 1, b: 1.5 } ).toArray().length );
assert.eq( 4, t.find().min( { a: 1, b: 2 } ).hint( { a: 1, b: -1 } ).toArray().length );
assert.eq( 4, t.find().max( { a: 2, b: 0.5 } ).hint( { a: 1, b: -1 } ).toArray().length );
assert.eq( 1, t.find().min( { a: 2, b: 1 } ).hint( { a: 1, b: -1 } ).toArray().length );
assert.eq( 1, t.find().max( { a: 1, b: 1.5 } ).hint( { a: 1, b: -1 } ).toArray().length );

// hint doesn't match
assert.throws( function() { t.find().min( { a: 1 } ).hint( { a: 1, b: -1 } ).toArray() } );
assert.throws( function() { t.find().min( { a: 1, b: 1 } ).max( { a: 1 } ).hint( { a: 1, b: -1 } ).toArray() } );
assert.throws( function() { t.find().min( { b: 1 } ).max( { a: 1, b: 2 } ).hint( { a: 1, b: -1 } ).toArray() } );
assert.throws( function() { t.find().min( { a: 1 } ).hint( { $natural: 1 } ).toArray() } );
assert.throws( function() { t.find().max( { a: 1 } ).hint( { $natural: 1 } ).toArray() } );

