t = db.objid2;
t.drop();

t.save( { _id : 517 , a : "hello" } )

assert.eq( t.findOne().a , "hello" );
assert.eq( t.findOne()._id , 517 );
