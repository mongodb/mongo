(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  jsTest.log('Testing running import with bad command line options');

  var toolTest = getToolTest('bad_options');
  var db1 = toolTest.db;

  // Make a dummy file to import by writing a test collection and exporting it
  assert.eq(0, db1.c.count(), "setup1");
  db1.c.save({a: 1, b: 2, c: 3});
  db1.c.save({a: 4, b: 5, c: 6});
  assert.eq(2, db1.c.count(), "setup2");

  toolTest.runTool("export",
      "--out", toolTest.extFile,
      "-d", toolTest.baseName,
      "-c", db1.c.getName());

  // also make a CSV version of it
  toolTest.runTool("export",
      "--out", toolTest.extFile + ".csv",
      "-d", toolTest.baseName,
      "-c", db1.c.getName(),
      "--csv",
      "--fields", "a,b,c");
  db1.c.drop();
  assert.eq(0, db1.c.count(), "after drop", "-d", toolTest.baseName, "-c", "foo");

  // verify that the normal sane case works
  var ret = toolTest.runTool("import",
      "--file", toolTest.extFile,
      "-d", "test",
      "-c", "test");
  assert.eq(ret, 0);

  // verify that the a lower case json type works
  ret = toolTest.runTool("import",
      "--file", toolTest.extFile,
      "-d", "test",
      "-c", "test",
      "--type=json");
  assert.eq(ret, 0);

  // verify that the a upper case json type works
  ret = toolTest.runTool("import",
      "--file", toolTest.extFile,
      "-d", "test",
      "-c", "test",
      "--type=JSON");
  assert.eq(ret, 0);

  // verify that the a csv type specifier failes to load a json file
  ret = toolTest.runTool("import",
      "--file", toolTest.extFile,
      "-d", "test",
      "-c", "test",
      "--type=csv",
      "-f", "a,b,c");
  assert.eq(ret, 1);

  // verify that the a lower case csv type works
  ret = toolTest.runTool("import",
      "--file", toolTest.extFile+".csv",
      "-d", "test",
      "-c", "test",
      "--type=csv",
      "-f", "a,b,c");
  assert.eq(ret, 0);

  // verify that the a upper case csv type works
  ret = toolTest.runTool("import",
      "--file", toolTest.extFile+".csv",
      "-d", "test",
      "-c", "test",
      "--type=CSV",
      "-f", "a,b,c");
  assert.eq(ret, 0);

  // verify that the a mixed case csv type works
  ret = toolTest.runTool("import",
      "--file", toolTest.extFile+".csv",
      "-d", "test",
      "-c", "test",
      "--type=cSv",
      "-f", "a,b,c");
  assert.eq(ret, 0);

  var testDb = db1.c.getDB().getSiblingDB("test");
  assert.eq.soon(11, testDb.test.count.bind(testDb.test), "test.test should have 11 records");
  testDb.test.drop();

  toolTest.stop();
}());
