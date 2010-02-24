t = db.geo5;
t.drop();

t.insert( { p : [ 0,0 ] } )
t.ensureIndex( { p : "2d" } )

res = t.runCommand( "geo2d" , { near : [1,1] } );
assert.eq( 1 , res.results.length , "A1" );

