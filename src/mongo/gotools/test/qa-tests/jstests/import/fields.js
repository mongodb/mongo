(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  jsTest.log('Testing running import with headerline');

  formats = ["csv", "tsv"];

  var checkCollectionContents = function(coll) {
    var importedDoc = coll.findOne({"a": "foo"});
    delete importedDoc["_id"];
    assert.docEq(importedDoc, {a: "foo", b: "bar", c: {xyz: "blah"}, d: {hij: {lkm: "qwz"}}});
    assert.eq(coll.count(), 3);
  };

  var reset = function(coll) {
    coll.drop();
    assert.eq(coll.count(), 0);
  };

  var toolTest = getToolTest("import_fields");
  var db1 = toolTest.db;
  var commonToolArgs= getCommonToolArguments();
  for (var i=0; i<formats.length; i++) {
    var format=formats[i];

    var c = db1.c.getDB().getSiblingDB(format + "testdb")[format+"testcoll"];
    // check that headerline uses the correct headers
    var ret = toolTest.runTool.apply(toolTest, ["import", "--file",
        "jstests/import/testdata/" +format+"_header." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--headerline"]
      .concat(commonToolArgs));

    checkCollectionContents(c);
    reset(c);

    // check that the fields can be specified with --fields
    ret = toolTest.runTool.apply(toolTest, ["import", "--file",
        "jstests/import/testdata/" +format+"_noheader." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--fields", "a,b,c.xyz,d.hij.lkm"]
      .concat(commonToolArgs));
    checkCollectionContents(c);
    reset(c);

    // check that the fields can be specified with --fieldsFile
    ret = toolTest.runTool.apply(toolTest, ["import", "--file",
        "jstests/import/testdata/" +format+"_noheader." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--fieldFile", "jstests/import/testdata/fieldfile"]
      .concat(commonToolArgs));
    checkCollectionContents(c);
    // check that without --ignoreBlanks, the empty field is just blank string
    assert.eq(c.findOne({a: "bob"}).b, "");
    reset(c);

    // check that --ignoreBlanks causes empty fields to be omitted
    ret = toolTest.runTool.apply(toolTest, ["import", "--file",
        "jstests/import/testdata/" + format + "_noheader." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--fieldFile", "jstests/import/testdata/fieldfile",
        "--ignoreBlanks"]
      .concat(commonToolArgs));
    assert.eq(c.findOne({a: "bob"}).b, undefined);
    reset(c);

    // when --fieldFile, --fields, and --headerline are all omitted,
    // import should fail
    ret = toolTest.runTool.apply(toolTest, ["import", "--file",
        "jstests/import/testdata/" + format + "_noheader." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll"]
      .concat(commonToolArgs));
    assert.neq(ret, 0);
    reset(c);

  }

  var c2 = db1.c.getDB().getSiblingDB("testdb")["extrafields"];
  // check that extra fields are created as expected
  ret = toolTest.runTool.apply(toolTest, ["import", "--file",
      "jstests/import/testdata/extrafields.csv",
      "--type=csv",
      "--db", c2.getDB().toString(),
      "--collection", c2.getName(),
      "--fieldFile", "jstests/import/testdata/fieldfile"]
    .concat(commonToolArgs));

  var importedDoc = c2.findOne({"a": "one"});
  assert.eq(importedDoc.field4, "extra1");
  assert.eq(importedDoc.field5, "extra2");
  assert.eq(importedDoc.field6, "extra3");
  reset(c2);

  toolTest.stop();
}());
