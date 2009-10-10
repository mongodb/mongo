// dumprestore1.js

t = new ToolTest( "dumprestore1" );

c = t.startDB( "foo" );
assert.eq( 0 , c.count() , "setup1" );
c.save( { a : 22 } );
assert.eq( 1 , c.count() , "setup2" );

t.runTool( "dump" , "--out" , t.ext );

c.drop();
assert.eq( 0 , c.count() , "after drop" );

t.runTool( "restore" , "--dir" , t.ext );
assert.soon( "c.findOne()" , "no data after sleep" );
assert.eq( 1 , c.count() , "after restore 2" );
assert.eq( 22 , c.findOne().a , "after restore 2" );

t.stop();
