

t = db.index_check3;
t.drop();



t.save( { a : 1 } );
t.save( { a : 2 } );
t.save( { a : 3 } );
t.save( { a : "z" } );

assert.eq( 1 , t.find( { a : { $lt : 2 } } ).itcount() , "A" );
assert.eq( 1 , t.find( { a : { $gt : 2 } } ).itcount() , "A" );

t.ensureIndex( { a : 1 } );

assert.eq( 1 , t.find( { a : { $lt : 2 } } ).itcount() , "A" );
assert.eq( 1 , t.find( { a : { $gt : 2 } } ).itcount() , "A" );

t.drop();

for ( var i=0; i<100; i++ ){
    var o = { i : i };
    if ( i % 2 == 0 )
        o.foo = i;
    t.save( o );
}

t.ensureIndex( { foo : 1 } );

printjson( t.find( { foo : { $lt : 50 } } ).explain() );
assert.gt( 30 , t.find( { foo : { $lt : 50 } } ).explain().nscanned , "lt" )
printjson( t.find( { foo : { $gt : 50 } } ).explain() );
assert.gt( 30 , t.find( { foo : { $gt : 50 } } ).explain().nscanned , "gt" )

