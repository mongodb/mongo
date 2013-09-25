

t = db.index_check3;
t.drop();



t.save( { a : 1 } );
t.save( { a : 2 } );
t.save( { a : 3 } );
t.save( { a : "z" } );

assert.eq( 1 , t.find( { a : { $lt : 2 } } ).itcount() , "A" );
assert.eq( 1 , t.find( { a : { $gt : 2 } } ).itcount() , "B" );

t.ensureIndex( { a : 1 } );

assert.eq( 1 , t.find( { a : { $lt : 2 } } ).itcount() , "C" );
assert.eq( 1 , t.find( { a : { $gt : 2 } } ).itcount() , "D" );

t.drop();

for ( var i=0; i<100; i++ ){
    var o = { i : i };
    if ( i % 2 == 0 )
        o.foo = i;
    t.save( o );
}

t.ensureIndex( { foo : 1 } );

//printjson( t.find( { foo : { $lt : 50 } } ).explain() );
assert.gt( 30 , t.find( { foo : { $lt : 50 } } ).explain().nscanned , "lt" );
//printjson( t.find( { foo : { $gt : 50 } } ).explain() );
assert.gt( 30 , t.find( { foo : { $gt : 50 } } ).explain().nscanned , "gt" );


t.drop();
t.save( {i:'a'} );
for( var i=0; i < 10; ++i ) {
    t.save( {} );
}

t.ensureIndex( { i : 1 } );

//printjson( t.find( { i : { $lte : 'a' } } ).explain() );
assert.gt( 3 , t.find( { i : { $lte : 'a' } } ).explain().nscanned , "lte" );
//printjson( t.find( { i : { $gte : 'a' } } ).explain() );
// bug SERVER-99
assert.gt( 3 , t.find( { i : { $gte : 'a' } } ).explain().nscanned , "gte" );
assert.eq( 1 , t.find( { i : { $gte : 'a' } } ).count() , "gte a" );
assert.eq( 1 , t.find( { i : { $gte : 'a' } } ).itcount() , "gte b" );
assert.eq( 1 , t.find( { i : { $gte : 'a' } } ).sort( { i : 1 } ).count() , "gte c" );
assert.eq( 1 , t.find( { i : { $gte : 'a' } } ).sort( { i : 1 } ).itcount() , "gte d" );

t.save( { i : "b" } );

assert.gt( 3 , t.find( { i : { $gte : 'a' } } ).explain().nscanned , "gte" );
assert.eq( 2 , t.find( { i : { $gte : 'a' } } ).count() , "gte a2" );
assert.eq( 2 , t.find( { i : { $gte : 'a' } } ).itcount() , "gte b2" );
assert.eq( 2 , t.find( { i : { $gte : 'a' , $lt : MaxKey } } ).itcount() , "gte c2" );
assert.eq( 2 , t.find( { i : { $gte : 'a' , $lt : MaxKey } } ).sort( { i : -1 } ).itcount() , "gte d2" );
assert.eq( 2 , t.find( { i : { $gte : 'a' , $lt : MaxKey } } ).sort( { i : 1 } ).itcount() , "gte e2" );
