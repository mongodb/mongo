
t = db.index_arr1
t.drop()

t.insert( { _id : 1 , a : 5 , b : [] } )
assert.eq( 1 , t.find( { a : 5 } ).itcount() , "A" )

t.ensureIndex( { a : 1 , "b.x" : 1 } )
t.insert( { _id : 1 , a : 5 , b : [] } )
// assert.eq( 1 , t.find( { a : 5 } ).itcount() , "B" ); // SERVER-1082
