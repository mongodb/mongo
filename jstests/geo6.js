
t = db.geo6;
t.drop();

t.ensureIndex( { loc : "2d" } );

t.insert( { _id : 1 , loc : [ 1 , 1 ] } )
t.insert( { _id : 2 , loc : [ 1 , 2 ] } )
t.insert( { _id : 3 } )

assert.eq( 3 , t.find().itcount() , "A1" )
assert.eq( 2 , t.find().hint( { loc : "2d" } ).itcount() , "A2" )
assert.eq( 2 , t.find( { loc : { $near : [50,50] } } ).itcount() , "A3" )
