
t = db.update_addToSet3
t.drop()

t.insert( { _id : 1 } )

t.update( { _id : 1 } , { $addToSet : { a : { $each : [ 6 , 5 , 4 ] } } } )
assert.eq( t.findOne() , { _id : 1 , a : [ 6 , 5 , 4 ] } , "A1" )

t.update( { _id : 1 } , { $addToSet : { a : { $each : [ 3 , 2 , 1 ] } } } )
assert.eq( t.findOne() , { _id : 1 , a : [ 6 , 5 , 4 , 3 , 2 , 1 ] } , "A2" )

t.update( { _id : 1 } , { $addToSet : { a : { $each : [ 4 , 7 , 9 , 2 ] } } } )
assert.eq( t.findOne() , { _id : 1 , a : [ 6 , 5 , 4 , 3 , 2 , 1 , 7 , 9 ] } , "A3" )

t.update( { _id : 1 } , { $addToSet : { a : { $each : [ 12 , 13 , 12 ] } } } )
assert.eq( t.findOne() , { _id : 1 , a : [ 6 , 5 , 4 , 3 , 2 , 1 , 7 , 9 , 12 , 13 ] } , "A4" )

