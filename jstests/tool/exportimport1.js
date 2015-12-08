// exportimport1.js

t = new ToolTest( "exportimport1" );

c = t.startDB( "foo" );
assert.eq( 0 , c.count() , "setup1" );
var arr = ["x", undefined, "y", undefined];
c.save( { a : 22 , b : arr} );
assert.eq( 1 , c.count() , "setup2" );

t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );;

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo" );
assert.soon( "c.findOne()" , "no data after sleep" );
assert.eq( 1 , c.count() , "after restore 2" );
var doc = c.findOne();
assert.eq( 22 , doc.a , "after restore 2" );
for (var i=0; i<arr.length; i++) {
    assert.eq( arr[i], doc.b[i] , "after restore array: "+i );
}

// now with --jsonArray

t.runTool( "export" , "--jsonArray" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );;

t.runTool( "import" , "--jsonArray" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo" );
assert.soon( "c.findOne()" , "no data after sleep" );
assert.eq( 1 , c.count() , "after restore 2" );
assert.eq( 22 , c.findOne().a , "after restore 2" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );

arr = ["a", undefined, "c"];
c.save({a : arr});
assert.eq( 1 , c.count() , "setup2" );
t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo" );
c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );;

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo" );
assert.soon( "c.findOne()" , "no data after sleep" );
assert.eq( 1 , c.count() , "after restore 2" );
var doc = c.findOne();
for (var i=0; i<arr.length; i++) {
    assert.eq( arr[i], doc.a[i] , "after restore array: "+i );
}


t.stop();
