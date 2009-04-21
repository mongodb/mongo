// Test key uniqueness

t = db.jstests_index8;
t.drop();

t.ensureIndex( { a: 1 } );
t.ensureIndex( { b: 1 }, true );
t.ensureIndex( { c: 1 }, [ false, "cIndex" ] );

checkIndexes = function() {
//    printjson( db.system.indexes.find( { ns: "test.jstests_index8" } ).toArray() );
    indexes = db.system.indexes.find( { ns: "test.jstests_index8" } ).sort( { key: 1 } );
    assert( !indexes[ 0 ].unique );
    assert( indexes[ 1 ].unique );
    assert( !indexes[ 2 ].unique );
    assert.eq( "cIndex", indexes[ 2 ].name );
}

checkIndexes();

t.reIndex();
checkIndexes();

t.save( { a: 2, b: 1 } );
t.save( { a: 2 } );
assert.eq( 2, t.find().count() );

t.save( { b: 4 } );
t.save( { b: 4 } );
assert.eq( 3, t.find().count() );

t.drop();
t.ensureIndex( { a: 1, b: -1 }, true );
t.save( { a: 2, b: 3 } );
t.save( { a: 2, b: 3 } );
t.save( { a: 2, b: 4 } );
t.save( { a: 1, b: 3 } );
assert.eq( 3, t.find().count() );

t.drop();
t.ensureIndex( { a: 1 }, true );
t.save( { a: [ 2, 3 ] } );
t.save( { a: 2 } );
assert.eq( 1, t.find().count() );
