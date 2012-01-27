
t = db.evalb;
t.drop();

t.save( { x : 3 } );

assert.eq( 3, db.eval( function(){ return db.evalb.findOne().x; } ) , "A" );

db.setProfilingLevel( 2 );

assert.eq( 3, db.eval( function(){ return db.evalb.findOne().x; } ) , "B" );

o = db.system.profile.find( { "command.$eval" : { $exists : true } } ).sort( { $natural : -1 } ).limit(1).next();
assert( tojson(o).indexOf( "findOne().x" ) > 0 , "C : " + tojson( o ) )

db.setProfilingLevel( 0 );

