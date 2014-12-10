(function() {
    jsTest.log('Testing running import with headerline');

    formats = ["csv", "tsv"]

    var checkCollectionContents = function(coll){
      var importedDoc = coll.findOne({"a":"foo"})
      delete importedDoc["_id"]
      assert.docEq(importedDoc, {a:"foo", b:"bar",c:{xyz:"blah"}, d:{hij:{lkm:"qwz"}}})
      assert.eq(c.count(), 3)
    }

    var reset = function(coll){
      coll.drop()
      assert.eq(coll.count(), 0)
    }

    var toolTest = new ToolTest('import');
    var db1 = toolTest.startDB('foo');
    for(var i=0;i<formats.length;i++){
      var format=formats[i]

      var c = db1.c.getDB().getSiblingDB(format + "testdb")[format+"testcoll"]
      //check that headerline uses the correct headers
      var ret = toolTest.runTool("import", "--file",
        "jstests/import/testdata/" +format+"_header." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--headerline")

      checkCollectionContents(c)
      reset(c)

      // check that the fields can be specified with --fields
      var ret = toolTest.runTool("import", "--file",
        "jstests/import/testdata/" +format+"_noheader." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--fields", "a,b,c.xyz,d.hij.lkm")
      checkCollectionContents(c)
      reset(c)

      // check that the fields can be specified with --fieldsFile
      var ret = toolTest.runTool("import", "--file",
        "jstests/import/testdata/" +format+"_noheader." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--fieldFile", "jstests/import/testdata/fieldfile")
      checkCollectionContents(c)
      // check that without --ignoreBlanks, the empty field is just blank string
      assert.eq(c.findOne({a:"bob"}).b, "")
      reset(c)

      // check that --ignoreBlanks causes empty fields to be omitted
      var ret = toolTest.runTool("import", "--file",
        "jstests/import/testdata/" + format + "_noheader." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--fieldFile", "jstests/import/testdata/fieldfile",
        "--ignoreBlanks"
        )
      assert.eq(c.findOne({a:"bob"}).b, undefined)
      reset(c)
    }


    var c2 = db1.c.getDB().getSiblingDB("testdb")["extrafields"]
    // check that extra fields are created as expected
    var ret = toolTest.runTool("import", "--file",
      "jstests/import/testdata/extrafields.csv",
      "--type=csv",
      "--db", c2.getDB().toString(),
      "--collection", c2.getName(),
      "--fieldFile", "jstests/import/testdata/fieldfile")

    var importedDoc = c2.findOne({"a":"one"})
    assert.eq(importedDoc.field4, "extra1")
    assert.eq(importedDoc.field5, "extra2")
    assert.eq(importedDoc.field6, "extra3")
    reset(c2)



    toolTest.stop();
}());
