
t = db.idhack
t.drop()


t.insert( { _id : { x : 1 } , z : 1 } )
t.insert( { _id : { x : 2 } , z : 2 } )
t.insert( { _id : { x : 3 } , z : 3 } )
t.insert( { _id : 1 , z : 4 } )
t.insert( { _id : 2 , z : 5 } )
t.insert( { _id : 3 , z : 6 } )

assert.eq( 2 , t.findOne( { _id : { x : 2 } } ).z , "A1" )
assert.eq( 2 , t.find( { _id : { $gte : 2 } } ).count() , "A2" )
assert.eq( 2 , t.find( { _id : { $gte : 2 } } ).itcount() , "A3" )

t.update( { _id : { x : 2 } } , { $set : { z : 7 } } )
assert.eq( 7 , t.findOne( { _id : { x : 2 } } ).z , "B1" )

t.update( { _id : { $gte : 2 } } , { $set : { z : 8 } } , false , true )
assert.eq( 4 , t.findOne( { _id : 1 } ).z , "C1" )
assert.eq( 8 , t.findOne( { _id : 2 } ).z , "C2" )
assert.eq( 8 , t.findOne( { _id : 3 } ).z , "C3" )
