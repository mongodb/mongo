// dumprestore2.js

t = new ToolTest( "dumprestore2" );

c = t.startDB( "foo" );
assert.eq( 0 , c.count() , "setup1" );
c.save( { a : 22 } );
assert.eq( 1 , c.count() , "setup2" );
t.stop();

// SERVER-2501 on Windows the mongod may still be running at this point, so we wait for it to stop.
sleep( 5000 );

t.runTool( "dump" , "--dbpath" , t.dbpath , "--out" , t.ext );

resetDbpath( t.dbpath );
assert.eq( 0 , listFiles( t.dbpath ).length , "clear" );

t.runTool( "restore" , "--dbpath" , t.dbpath , "--dir" , t.ext );

listFiles( t.dbpath ).forEach( printjson )

c = t.startDB( "foo" );
assert.soon( "c.findOne()" , "no data after startup" );
assert.eq( 1 , c.count() , "after restore 2" );
assert.eq( 22 , c.findOne().a , "after restore 2" );

t.stop();

