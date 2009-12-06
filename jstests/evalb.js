
t = db.evalb;
t.drop();

t.save( { x : 3 } );

assert.eq( 3, db.eval( function(){ return db.evalb.findOne().x; } ) , "A" );

db.setProfilingLevel( 2 );

assert.eq( 3, db.eval( function(){ return db.evalb.findOne().x; } ) , "B" );

db.setProfilingLevel( 0 );

