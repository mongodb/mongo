// exportimport1.js

t = new ToolTest( "exportimport1" );

c = t.startDB( "foo" );
assert.eq( 0 , c.count() , "setup1" );
c.save( { a : 22 } );
assert.eq( 1 , c.count() , "setup2" );

t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );;

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo" );
assert.soon( "c.findOne()" , "no data after sleep" );
assert.eq( 1 , c.count() , "after restore 2" );
assert.eq( 22 , c.findOne().a , "after restore 2" );

t.stop();
