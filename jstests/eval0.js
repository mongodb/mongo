
assert.eq( 17 , db.eval( function(){ return 11 + 6; } ) , "A" );
assert.eq( 17 , db.eval( function( x ){ return 10 + x; } , 7 ) , "B" );
