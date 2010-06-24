
t = db.upsert1;
t.drop();

t.update( { x : 1 } , { $inc : { y : 1 } } , true );
l = db.getLastErrorCmd();
assert( l.upserted , "A1" );
assert.eq( l.upserted.str , t.findOne()._id.str , "A2" );

t.update( { x : 2 } , { x : 2 , y : 3 } , true );
l = db.getLastErrorCmd();
assert( l.upserted , "B1" );
assert.eq( l.upserted.str , t.findOne( { x : 2 } )._id.str , "B2" );
assert.eq( 2 , t.find().count() , "B3" );
