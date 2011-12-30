t = db.jstests_js7;
t.drop();

assert.eq( 17 , db.eval( function(){ return args[0]; } , 17 ) );

assert.eq( 17 , db.eval( function( foo ){ return foo; } , 17 ) );

