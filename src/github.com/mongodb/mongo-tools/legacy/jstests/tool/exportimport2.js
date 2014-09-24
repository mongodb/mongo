// exportimport2.js

t = new ToolTest( "exportimport2" );

c = t.startDB( "foo" );
assert.eq( 0 , c.count() , "setup1" );
c.save( { a : 22 } );
assert.eq( 1 , c.count() , "setup2" );
t.stop();

t.runTool( "export" , "--dbpath" , t.dbpath , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo" );

resetDbpath( t.dbpath );
assert.eq( 0 , listFiles( t.dbpath ).length , "clear" );

t.runTool( "import" , "--dbpath" , t.dbpath , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo" );

c = t.startDB( "foo" );
assert.soon( "c.findOne()" , "no data after startup" );
assert.eq( 1 , c.count() , "after restore 2" );
assert.eq( 22 , c.findOne().a , "after restore 2" );

t.stop();

