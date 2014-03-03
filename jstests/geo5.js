t = db.geo5;
t.drop();

t.insert( { p : [ 0,0 ] } )
t.ensureIndex( { p : "2d" } )

res = t.runCommand( "geoNear" , { near : [1,1] } );
assert.eq( 1 , res.results.length , "A1" );

t.insert( { p : [ 1,1 ] } )
t.insert( { p : [ -1,-1 ] } )
res = t.runCommand( "geoNear" , { near : [50,50] } );
assert.eq( 3 , res.results.length , "A2" );

t.insert( { p : [ -1,-1 ] } )
res = t.runCommand( "geoNear" , { near : [50,50] } );
assert.eq( 4 , res.results.length , "A3" );

