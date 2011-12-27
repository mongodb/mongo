// exportimport3.js

t = new ToolTest( "exportimport3" );

c = t.startDB( "foo" );
assert.eq( 0 , c.count() , "setup1" );
c.save({a:1})
c.save({a:2})
c.save({a:3})
c.save({a:4})
c.save({a:5})

assert.eq( 5 , c.count() , "setup2" );


t.runTool( "export" , "--jsonArray" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );;

t.runTool( "import" , "--jsonArray" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo" );

assert.soon( "c.findOne()" , "no data after sleep" );
assert.eq( 5 , c.count() , "after restore 2" );


t.stop();
