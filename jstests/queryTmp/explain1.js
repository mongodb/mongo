
t = db.explain1;
t.drop();

for ( var i=0; i<100; i++ ){
    t.save( { x : i } );
}

q = { x : { $gt : 50 } };

assert.eq( 49 , t.find( q ).count() , "A" );
assert.eq( 49 , t.find( q ).itcount() , "B" );
assert.eq( 20 , t.find( q ).limit(20).itcount() , "C" );

t.ensureIndex( { x : 1 } );

assert.eq( 49 , t.find( q ).count() , "D" );
assert.eq( 49 , t.find( q ).itcount() , "E" );
assert.eq( 20 , t.find( q ).limit(20).itcount() , "F" );

assert.eq( 49 , t.find(q).explain().n , "G" );
assert.eq( 20 , t.find(q).limit(20).explain().n , "H" );
assert.eq( 20 , t.find(q).limit(-20).explain().n , "I" );
assert.eq( 49 , t.find(q).batchSize(20).explain().n , "J" );
