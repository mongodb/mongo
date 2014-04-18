
t = db.inc3;

t.drop();
t.save( { _id : 1 , z : 1 , a : 1 } );
t.update( {} , { $inc : { z : 1 , a : 1 } } );
t.update( {} , { $inc : { a : 1 , z : 1 } } );
assert.eq( { _id : 1 , z : 3 , a : 3 } , t.findOne() , "A" )


t.drop();
t.save( { _id : 1 , a : 1 , z : 1 } );
t.update( {} , { $inc : { z : 1 , a : 1 } } );
t.update( {} , { $inc : { a : 1 , z : 1 } } );
assert.eq( { _id : 1 , a : 3 , z : 3 } , t.findOne() , "B" )

