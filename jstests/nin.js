t = db.jstests_nin;
t.drop();

doTest = function() {
    
    t.save( { a:[ 1,2,3 ] } );
    t.save( { a:[ 1,2,4 ] } );
    t.save( { a:[ 1,8,5 ] } );
    t.save( { a:[ 1,8,6 ] } );
    t.save( { a:[ 1,9,7 ] } );
    
    assert.eq( 5, t.find( { a: { $nin: [ 10 ] } } ).count() );
    assert.eq( 0, t.find( { a: { $ne: 1 } } ).count() );
    assert.eq( 0, t.find( { a: { $nin: [ 1 ] } } ).count() );
    assert.eq( 0, t.find( { a: { $nin: [ 1, 2 ] } } ).count() );
    assert.eq( 3, t.find( { a: { $nin: [ 2 ] } } ).count() );
    assert.eq( 3, t.find( { a: { $nin: [ 8 ] } } ).count() );
    assert.eq( 4, t.find( { a: { $nin: [ 9 ] } } ).count() );
    assert.eq( 4, t.find( { a: { $nin: [ 3 ] } } ).count() );
    assert.eq( 3, t.find( { a: { $nin: [ 2, 3 ] } } ).count() );
    
    t.save( { a: [ 2, 2 ] } );
    assert.eq( 3, t.find( { a: { $nin: [ 2, 2 ] } } ).count() );
    
    t.save( { a: [ [ 2 ] ] } );
    assert.eq( 4, t.find( { a: { $nin: [ 2 ] } } ).count() );    
    
    t.save( { a: [ { b: [ 10, 11 ] }, 11 ] } );
    assert.eq( 1, t.find( { 'a.b': { $nin: [ 10 ] } } ).count() );
    assert.eq( 0, t.find( { 'a.b': { $nin: [ [ 10, 11 ] ] } } ).count() );
    assert.eq( 7, t.find( { a: { $nin: [ 11 ] } } ).count() );

    t.save( { a: { b: [ 20, 30 ] } } );
    assert.eq( 1, t.find( { 'a.b': { $all: [ 20 ] } } ).count() );
    assert.eq( 1, t.find( { 'a.b': { $all: [ 20, 30 ] } } ).count() );
}

doTest();
t.drop();
t.ensureIndex( {a:1} );
doTest();
