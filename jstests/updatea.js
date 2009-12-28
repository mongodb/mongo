
t = db.updatea;
t.drop();

orig = { _id : 1 , a : [ { x : 1 , y : 2 } , { x : 10 , y : 11 } ] }

t.save( orig )

// SERVER-181
t.update( {} , { $set : { "a.0.x" : 3 } } )
orig.a[0].x = 3;
assert.eq( orig , t.findOne() , "A1" );

t.update( {} , { $set : { "a.1.z" : 17 } } )
orig.a[1].z = 17;
assert.eq( orig , t.findOne() , "A2" );

// SERVER-273
t.update( {} , { $unset : { "a.1.y" : 1 } } )
delete orig.a[1].y
assert.eq( orig , t.findOne() , "A3" );

// SERVER-333
t.drop();
orig = { _id : 1 , comments : [ { name : "blah" , rate_up : 0 , rate_ups : [] } ] }
t.save( orig );

t.update( {} , { $inc: { "comments.0.rate_up" : 1 } , $push: { "comments.0.rate_ups" : 99 } } )
orig.comments[0].rate_up++;
orig.comments[0].rate_ups.push( 99 )
assert.eq( orig , t.findOne() , "B1" )



