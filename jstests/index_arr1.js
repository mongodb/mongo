
t = db.index_arr1
t.drop()

t.insert( { _id : 1 , a : 5 , b : [ { x : 1 } ] } )
t.insert( { _id : 2 , a : 5 , b : [] } )
t.insert( { _id : 3 , a : 5  } )

assert.eq( 3 , t.find( { a : 5 } ).itcount() , "A" )

t.ensureIndex( { a : 1 , "b.x" : 1 } )

//t.find().sort( { a : 1 } )._addSpecial( "$returnKey" , 1 ).forEach( printjson )
//t.find( { a : 5 } ).forEach( printjson )

assert.eq( 3 , t.find( { a : 5 } ).itcount() , "B" ); // SERVER-1082
