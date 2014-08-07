
t = db.index_check7
t.drop()

for ( var i=0; i<100; i++ )
    t.save( { x : i } )

t.ensureIndex( { x : 1 } )
assert.eq( 1 , t.find( { x : 27 } ).explain().nscanned , "A" )

t.ensureIndex( { x : -1 } )
assert.eq( 1 , t.find( { x : 27 } ).explain().nscanned , "B" )

assert.eq( 40 , t.find( { x : { $gt : 59 } } ).explain().nscanned , "C" );

