(function() {
    jsTest.log('Testing running import with various data types');

    var testDoc = {
      _id : ObjectId(),
      a : BinData(0,"e8MEnzZoFyMmD7WSHdNrFJyEk8M="),
      d : "this is a string",
      e : ["this is an ", 2, 23.5, "array with various types in it"],
      f : {"this is": "an embedded doc"},
      g : function(){print("hey sup")},
      h : null,
      i : true,
      j : false,
      k : NumberLong(10000),
      l : MinKey(),
      k : MaxKey(),
    }


    var toolTest = new ToolTest('import');
    var db1 = toolTest.startDB('foo');

    //Make a dummy file to import by writing a test collection and exporting it
    assert.eq( 0 , db1.c.count() , "setup1" );
    db1.c.save(testDoc)
    toolTest.runTool( "export" , "--out" , toolTest.extFile , "-d" , toolTest.baseName , "-c" , db1.c.getName());

    var ret = toolTest.runTool("import", "--file",toolTest.extFile, "--db", "imported", "--collection", "testcoll2")
    var postImportDoc = db1.c.getDB().getSiblingDB("imported").testcoll2.findOne()


    printjson(postImportDoc)

    docKeys = Object.keys(testDoc)
    for(var i=0;i<docKeys.length;i++){
      jsTest.log("checking field", docKeys[i])
      assert.eq(testDoc[docKeys[i]], postImportDoc[docKeys[i]], "imported field " + docKeys[i] + " does not match original")
    }


    toolTest.stop();
}());
