
s = db.system.js;
s.remove({});
assert.eq( 0 , s.count() , "setup - A" );

s.save( { _id : "x" , value : "3" } );
assert.isnull( db.getLastError() , "setup - B" );
assert.eq( 1 , s.count() , "setup - C" );

s.remove( { _id : "x" } );
assert.eq( 0 , s.count() , "setup - D" );
s.save( { _id : "x" , value : "4" } );
assert.eq( 1 , s.count() , "setup - E" );

assert.eq( 4 , s.findOne( { _id : "x" } ).value , "E2 " );

assert.eq( 4 , s.findOne().value , "setup - F" );
s.update( { _id : "x" } , { $set : { value : 5  } } );
assert.eq( 1 , s.count() , "setup - G" );
assert.eq( 5 , s.findOne().value , "setup - H" );

assert.eq( 5 , db.eval( "return x" ) , "exec - 1 " );

s.update( { _id : "x" } , { $set : { value : 6  } } );
assert.eq( 1 , s.count() , "setup2 - A" );
assert.eq( 6 , s.findOne().value , "setup - B" );

assert.eq( 6 , db.eval( "return x" ) , "exec - 2 " );



s.insert( { _id : "bar" , value : function( z ){ return 17 + z; } } );
assert.eq( 22 , db.eval( "return bar(5);"  ) , "exec - 3 " );


