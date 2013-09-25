
t = db.proj_key1;
t.drop();

as = []

for ( i=0; i<10; i++ ){
    as.push( { a : i } )
    t.insert( { a : i , b : i } );
}

// NEW QUERY EXPLAIN
assert.eq(t.find( {} , { a : 1 } ).itcount(), 10);

t.ensureIndex( { a : 1 } )

// NEW QUERY EXPLAIN
assert.eq(t.find( { a : { $gte : 0 } } , { a : 1 , _id : 0 } ).itcount(), 10)

// NEW QUERY EXPLAIN
assert.eq(t.find( { a : { $gte : 0 } } , { a : 1 } ).itcount(), 10);

// assert( t.find( {} , { a : 1 , _id : 0 } ).explain().indexOnly , "A4" ); // T.itcount(), 10);
assert.eq( as , t.find( { a : { $gte : 0 } } , { a : 1 , _id : 0 } ).toArray() , "B1" )
assert.eq( as , t.find( { a : { $gte : 0 } } , { a : 1 , _id : 0 } ).batchSize(2).toArray() , "B1" )





