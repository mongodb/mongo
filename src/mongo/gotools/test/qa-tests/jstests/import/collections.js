(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  jsTest.log('Testing running import with bad command line options');

  var toolTest = getToolTest('import');
  var db1 = toolTest.db;
  var commonToolArgs = getCommonToolArguments();


  // Make a dummy file to import by writing a test collection and exporting it
  assert.eq(0, db1.c.count(), "setup1");
  db1.c.save({a: 1, b: 2, c: 3});
  db1.c.save({a: 4, b: 5, c: 6});
  assert.eq(2, db1.c.count(), "setup2");

  toolTest.runTool.apply(toolTest, ["export",
      "--out", toolTest.extFile,
      "-d", toolTest.baseName,
      "-c", db1.c.getName()]
    .concat(commonToolArgs));

  db1.c.drop();
  assert.eq(0, db1.c.count(), "after drop", "-d", toolTest.baseName, "-c", "foo");


  // copy the file to a file that contains the collection name
  removeFile("foo.blah.json");
  copyFile(toolTest.extFile, "foo.blah.json");

  // copy the file to a file that contains the collection name plus an extra extension (.backup)
  removeFile("foo.blah.json.backup");
  copyFile(toolTest.extFile, "foo.blah.json.backup");


  toolTest.runTool.apply(toolTest, ["import",
      "--file", "foo.blah.json"]
    .concat(commonToolArgs));
  assert.eq(db1.c.getDB().getSiblingDB("test").foo.blah.count(), 2,
      "importing file named after collection should insert to correct namespace");
  db1.c.getDB().getSiblingDB("test").foo.blah.drop();

  toolTest.runTool.apply(toolTest, ["import",
      "--file", "foo.blah.json.backup"]
    .concat(commonToolArgs));
  assert.eq(db1.c.getDB().getSiblingDB("test").foo.blah.json.count(), 2,
      "importing file with extra extension should still assume correct namespace");
  db1.c.getDB().getSiblingDB("test").foo.blah.json.drop();

  toolTest.runTool.apply(toolTest, ["import",
      "--file", "foo.blah.json",
      "--collection", "testcoll1"]
    .concat(commonToolArgs));
  assert.eq(db1.c.getDB().getSiblingDB("test").testcoll1.count(), 2,
      "importing --file with --collection should use correct collection name");
  db1.c.getDB().getSiblingDB("test").testcoll1.drop();

  toolTest.runTool.apply(toolTest, ["import",
      "foo.blah.json"]
    .concat(commonToolArgs));
  assert.eq(db1.c.getDB().getSiblingDB("test").foo.blah.count(), 2,
      "should be allowed to specify file as positional arg");
  db1.c.getDB().getSiblingDB("test").foo.blah.drop();

  toolTest.runTool.apply(toolTest, ["import",
      "foo.blah.json",
      "--db", "testdb2"]
    .concat(commonToolArgs));
  assert.eq(db1.c.getDB().getSiblingDB("testdb2").foo.blah.count(), 2,
      "should use database specified by --db");
  db1.c.getDB().getSiblingDB("testdb2").foo.blah.drop();

  toolTest.stop();
}());
