
t = db.proj_key1;
t.drop();

as = []

for ( i=0; i<10; i++ ){
    as.push( { a : i } )
    t.insert( { a : i , b : i } );
}

assert( ! t.find( {} , { a : 1 } ).explain().indexOnly , "A1" )

t.ensureIndex( { a : 1 } )

assert( t.find( { a : { $gte : 0 } } , { a : 1 , _id : 0 } ).explain().indexOnly , "A2" )

assert( ! t.find( { a : { $gte : 0 } } , { a : 1 } ).explain().indexOnly , "A3" ) // because id _id

// assert( t.find( {} , { a : 1 , _id : 0 } ).explain().indexOnly , "A4" ); // TODO: need to modify query optimier SERVER-2109

assert.eq( as , t.find( { a : { $gte : 0 } } , { a : 1 , _id : 0 } ).toArray() , "B1" )
assert.eq( as , t.find( { a : { $gte : 0 } } , { a : 1 , _id : 0 } ).batchSize(2).toArray() , "B1" )





