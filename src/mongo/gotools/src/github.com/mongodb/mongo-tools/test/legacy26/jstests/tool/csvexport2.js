// csvexport2.js

t = new ToolTest( "csvexport2" )

c = t.startDB( "foo" );

// This test is designed to test exporting of a CodeWithScope object.
// However, due to SERVER-3391, it is not possible to create a CodeWithScope object in the mongo shell,
// therefore this test does not work.  Once SERVER-3391 is resolved, this test should be un-commented out

//assert.eq( 0 , c.count() , "setup1" );

//c.insert({ a : 1 , b : Code("print(\"Hello \" + x);", {"x" : "World!"})})
//assert.eq( 1 , c.count() , "setup2" );
//t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo" , "--csv", "-f", "a,b")


//c.drop()

//assert.eq( 0 , c.count() , "after drop" )
//t.runTool("import", "--file", t.extFile, "-d", t.baseName, "-c", "foo", "--type", "csv", "--headerline");
//assert.soon ( 1 + " == c.count()", "after import");

//expected = { a : 1, b : "\"{ \"$code\" : print(\"Hello \" + x); ,  \"$scope\" : { \"x\" : \"World!\" } }"};
//actual = c.findOne()

//delete actual._id;
//assert.eq( expected, actual );


t.stop()