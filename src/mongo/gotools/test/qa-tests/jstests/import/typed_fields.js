(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  var formats = ["csv", "tsv"];
  var header = "a.string(),b.int32(),c.xyz.date_oracle(Month dd, yyyy HH24:mi:ss),c.noop.boolean(),d.hij.lkm.binary(hex)";
  var expectedDocs = [{
    a: "foo",
    b: 12,
    c: {
      xyz: ISODate("1997-06-02T15:24:00Z"),
      noop: true,
    },
    d: {hij: {lkm: BinData(0, "e8MEnzZoFyMmD7WSHdNrFJyEk8M=")}},
  }, {
    a: "bar",
    b: 24,
    c: {
      xyz: ISODate("2016-06-08T09:26:00Z"),
      noop: false,
    },
    d: {hij: {lkm: BinData(0, "dGVzdAo=")}},
  }];
  jsTest.log('Testing typed fields in CSV/TSV');

  var checkCollectionContents = function(coll) {
    var importedDoc = coll.findOne({a: "foo"});
    delete importedDoc["_id"];
    assert.docEq(importedDoc, expectedDocs[0]);
    importedDoc = coll.findOne({a: "bar"});
    delete importedDoc["_id"];
    assert.docEq(importedDoc, expectedDocs[1]);
    assert.eq(coll.count(), 2);
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
    var ret = toolTest.runTool.apply(toolTest, ["import",
        "--file", "jstests/import/testdata/typed_header." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--columnsHaveTypes",
        "--headerline"]
      .concat(commonToolArgs));

    checkCollectionContents(c);
    reset(c);

    // check that the fields can be specified with --fields
    ret = toolTest.runTool.apply(toolTest, ["import",
        "--file", "jstests/import/testdata/typed_noheader." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--columnsHaveTypes",
        "--fields", header]
      .concat(commonToolArgs));
    checkCollectionContents(c);
    reset(c);

    // check that the fields can be specified with --fieldsFile
    ret = toolTest.runTool.apply(toolTest, ["import",
        "--file", "jstests/import/testdata/typed_noheader." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--columnsHaveTypes",
        "--fieldFile", "jstests/import/testdata/typedfieldfile"]
      .concat(commonToolArgs));
    checkCollectionContents(c);
    reset(c);

    // when --fieldFile, --fields, and --headerline are all omitted,
    // import should fail
    ret = toolTest.runTool.apply(toolTest, ["import",
        "--file", "jstests/import/testdata/typed_noheader." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--columnsHaveTypes"]
      .concat(commonToolArgs));
    assert.neq(ret, 0);
    reset(c);

    // check that extra fields are created as expected
    ret = toolTest.runTool.apply(toolTest, ["import",
        "--file", "jstests/import/testdata/typed_extrafields." + format,
        "--type=" + format,
        "--db", format + "testdb",
        "--collection", format + "testcoll",
        "--columnsHaveTypes",
        "--fieldFile", "jstests/import/testdata/typedfieldfile"]
      .concat(commonToolArgs));

    var importedDoc = c.findOne({"a": "one"});
    assert.eq(importedDoc.field5, "extra1");
    assert.eq(importedDoc.field6, "extra2");
    reset(c);
  }

  toolTest.stop();
}());
