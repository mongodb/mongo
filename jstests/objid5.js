
t = db.objid5;
t.drop();

t.save( { _id : 5.5 } );
assert.eq( 18 , Object.bsonsize( t.findOne() ) , "A" );
