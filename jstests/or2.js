t = db.jstests_or2;
t.drop();

checkArrs = function( a, b, m ) {
    assert.eq( a.length, b.length, m );
    aStr = [];
    bStr = [];
    a.forEach( function( x ) { aStr.push( tojson( x ) ); } );
    b.forEach( function( x ) { bStr.push( tojson( x ) ); } );
    for ( i = 0; i < aStr.length; ++i ) {
        assert( -1 != bStr.indexOf( aStr[ i ] ), m );
    }
}

doTest = function( index ) {
    if ( index == null ) {
        index = true;
    }
    
    t.save( {_id:0,x:0,a:1} );
    t.save( {_id:1,x:0,a:2} );
    t.save( {_id:2,x:0,b:1} );
    t.save( {_id:3,x:0,b:2} );
    t.save( {_id:4,x:1,a:1,b:1} );
    t.save( {_id:5,x:1,a:1,b:2} );
    t.save( {_id:6,x:1,a:2,b:1} );
    t.save( {_id:7,x:1,a:2,b:2} );
    
    assert.throws( function() { t.find( { x:0,$or:"a" } ).toArray(); } );
    assert.throws( function() { t.find( { x:0,$or:[] } ).toArray(); } );
    assert.throws( function() { t.find( { x:0,$or:[ "a" ] } ).toArray(); } );
    
    a1 = t.find( { x:0, $or: [ { a : 1 } ] } ).toArray();
    checkArrs( [ { _id:0, x:0, a:1 } ], a1 );
    if ( index ) {
        assert( t.find( { x:0,$or: [ { a : 1 } ] } ).explain().cursor.match( /Btree/ ) );
    }
    
    a1b2 = t.find( { x:1, $or: [ { a : 1 }, { b : 2 } ] } ).toArray();
    checkArrs( [ { _id:4, x:1, a:1, b:1 }, { _id:5, x:1, a:1, b:2 }, { _id:7, x:1, a:2, b:2 } ], a1b2 );
    if ( index ) {
        assert( t.find( { x:0,$or: [ { a : 1 } ] } ).explain().cursor.match( /Btree/ ) );
    }
        
    t.drop();
    obj = {_id:0,x:10,a:[1,2,3]};
    t.save( obj );
    t.update( {x:10,$or:[ {a:2} ]}, {$set:{'a.$':100}} );
    assert.eq( obj, t.findOne() ); // no change
}

doTest( false );

t.ensureIndex( { x:1 } );
doTest();

t.drop();
t.ensureIndex( { x:1,a:1 } );
doTest();

t.drop();
t.ensureIndex( {x:1,b:1} );
doTest();

t.drop();
t.ensureIndex( {x:1,a:1,b:1} );
doTest();
