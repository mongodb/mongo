

a = db.rename3a
b = db.rename3b

a.drop();
b.drop()

a.save( { x : 1 } );
b.save( { x : 2 } );

assert.eq( 1 , a.findOne().x , "before 1a" );
assert.eq( 2 , b.findOne().x , "before 2a" );

res = b.renameCollection( a._shortName );
assert.eq( 0 , res.ok , "should fail: " + tojson( res ) );

assert.eq( 1 , a.findOne().x , "before 1b" );
assert.eq( 2 , b.findOne().x , "before 2b" );

res = b.renameCollection( a._shortName , true )
assert.eq( 1 , res.ok , "should succeed:" + tojson( res ) );

assert.eq( 2 , a.findOne().x , "after 1" );
assert.isnull( b.findOne() , "after 2" ); 
