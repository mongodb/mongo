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
      "--fields", "a,b,c",
      "--csv");
  db1.c.drop();
  assert.eq(0, db1.c.count(), "after drop", "-d", toolTest.baseName, "-c", "foo");

  // verify that the normal sane case works
  var ret = toolTest.runTool("import",
      "--file", toolTest.extFile,
      "-d", "test",
      "-c", "test");
  assert.eq(ret, 0);

  var testDb = db1.c.getDB().getSiblingDB("test");
  assert.eq.soon(2, testDb.test.count.bind(testDb.test), "test.test should have 2 records");
  testDb.test.drop();

  var testScenarios = [
      {args: [],
        desc: "importing with no args should fail"},

      {args: [toolTest.extFile, toolTest.extFile],
        desc: "importing with multiple positional args should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile, toolTest.extFile],
        desc: "specifying both a --file and a positional argument should fail"},

      {args: ["--db", "test", "-c", "test", "--file", "non-existent-file.json"],
        desc: "specifying a --file with a nonexistent filename should fail"},

      {args: ["--db", "test", "-c", "test", "--file", "."],
        desc: "specifying a --file with a directory name should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile, "--type", "bogus"],
        desc: "importing with an invalid --type should fail"},

      {args: ["--db", "x.y.z", "-c", "test", "--file", toolTest.extFile],
        desc: "importing with an invalid database name (. in name) should fail"},

      {args: ["--db", "$x", "-c", "test", "--file", toolTest.extFile],
        desc: "importing with an invalid database name ($ in name) should fail"},

      {args: ["--db", "test", "-c", "blah$asfsaf", "--file", toolTest.extFile],
        desc: "importing with an invalid collection name should fail"},

      {args: ["--db", "test", "-c", "blah$asfsaf", "--file", toolTest.extFile],
        desc: "importing with an invalid collection name should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile + ".csv", "--type=csv", "--fields", "a,$xz,b"],
        desc: "--fields containing a field containing a $ should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile, "--type=json", "--fields", "a,b"],
        desc: "specifying --fields with --json should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile + ".csv", "--headerline", "--fields", "a,b", "--type=csv"],
        desc: "specifying both --fields and --headerline should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile + ".csv", "--type=csv", "--fields", "a,b", "--fieldFile", toolTest.extFile + ".csv"],
        desc: "specifying both --fields and --fieldFile should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile + ".csv", "--type=csv", "--headerline", "--fieldFile", toolTest.extFile + ".csv"],
        desc: "specifying both --headerline and --fieldFile should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile + ".csv", "--type=csv", "--fields", "a,b,b"],
        desc: "--fields with duplicate field names should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile + ".csv", "--type=csv", "--fields", "a,b,b.c"],
        desc: "--fields with field names of overlapping structures should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile, "--type=csv", "--fields", "a,b,b.c"],
        desc: "--fields with field names of overlapping structures should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile, "--upsertFields", "a,$b"],
        desc: "invalid characters in upsertFields should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile, "--jsonArray"],
        desc: "using --jsonArray with a non-array input file should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile + ".csv", "--type=json"],
        desc: "using --type=json with invalid json should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile, "--type=csv", "--fields=a,b,c"],
        desc: "using --type=csv with invalid csv should fail"},

      {args: ["--db", "test", "-c", "test", "--file", toolTest.extFile, "--type=json", "--headerline"],
        desc: "using --type=json with headerline should fail"},
  ];

  for (var i=0; i<testScenarios.length; i++) {
    jsTest.log('Testing: ' + testScenarios[i].desc);
    ret = toolTest.runTool.apply(toolTest, ["import"].concat(testScenarios[i].args));
    assert.neq(0, ret, i + ": " + testScenarios[i].desc);
  }

  toolTest.stop();
}());
