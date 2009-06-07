

t = db.index_check3;
t.drop();

for ( var i=0; i<100; i++ ){
    var o = { i : i };
    if ( i % 2 == 0 )
        o.foo = i;
    t.save( o );
}

t.ensureIndex( { foo : 1 } );

assert.gt( 30 , t.find( { foo : { $lt : 50 } } ).explain().nscanned , "lt" )
//assert.gt( 30 , t.find( { foo : { $gt : 50 } } ).explain().nscanned , "gt" )

