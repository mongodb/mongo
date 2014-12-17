
t = db.count4;
t.drop();

for ( i=0; i<100; i++ ){
    t.save( { x : i } );
}

q = { x : { $gt : 25 , $lte : 75 } }

assert.eq( 50 , t.find( q ).count() , "A" );
assert.eq( 50 , t.find( q ).itcount() , "B" );

t.ensureIndex( { x : 1 } );

assert.eq( 50 , t.find( q ).count() , "C" );
assert.eq( 50 , t.find( q ).itcount() , "D" );
