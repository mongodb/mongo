(function() {
  jsTest.log('Testing running import with modes');

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  jsTest.log('Testing running import with bad command line cmdArgs');

  var toolTest = getToolTest('import');
  var db1 = toolTest.db;

  var db = db1.getSiblingDB("upserttest");
  db.dropDatabase();

  var commonToolArgs = [
    "--db", db.getName(),
    "--collection", db.c.getName(),
  ].concat(getCommonToolArguments());

  function testWithUpsertFields(expectMode, cmdArg) {
    // This works by applying update w/ query on the fields
    db.c.drop();
    var doc1_origin = {a: 1234, b: "000000", c: 222, x: "origin field"};
    var doc2_1_origin = {a: 4567, b: "111111", c: 333, x: "origin field"};
    db.c.insert(doc1_origin);
    db.c.insert(doc2_1_origin);
    assert.eq(db.c.count(), 2, "collection count should be 2 at setup");

    var argv = ["import",
        "--file", "jstests/import/testdata/upsert2.json",
        "--upsertFields", "a,c"];
    if (cmdArg) {
      argv.push(cmdArg);
    }
    argv = argv.concat(commonToolArgs);
    var ret = toolTest.runTool.apply(toolTest, argv);
    if (expectMode === "error") {
      return assert.neq(ret, 0);
    }
    assert.eq(ret, 0);

    var doc1 = db.c.findOne({a: 1234});
    var doc1_expect;
    delete doc1["_id"];
    switch (expectMode) {
    case "upsert":
      doc1_expect = {a: 1234, b: "blah", c: 222};
      break;
    case "merge":
      doc1_expect = {a: 1234, b: "blah", c: 222, x: "origin field"};
      break;
    default:
      throw new Error();
    }
    assert.docEq(doc1, doc1_expect);

    var doc2_1 = db.c.findOne({a: 4567, c: 333});
    var doc2_2 = db.c.findOne({a: 4567, c: 222});
    delete doc2_1["_id"];
    delete doc2_2["_id"];
    var doc2_1_expect, doc2_2_expect;
    switch (expectMode) {
    case "upsert":
      doc2_1_expect = {a: 4567, b: "yyy", c: 333};
      doc2_2_expect = {a: 4567, b: "asdf", c: 222};
      break;
    case "merge":
      doc2_1_expect = {a: 4567, b: "yyy", c: 333, x: "origin field"};
      doc2_2_expect = {a: 4567, b: "asdf", c: 222};
      break;
    default:
      throw new Error();
    }
    assert.docEq(doc2_1, doc2_1_expect);
    assert.docEq(doc2_2, doc2_2_expect);
  }

  function testWithoutUpsertFields(expectMode, cmdArg) {
    // This works by applying the update using _id
    db.c.drop();
    var docOrigin = [
      {_id: "one", a: "origin value", x: "origin field"},
      {_id: "two", a: "origin value 2", x: "origin field"},
    ];
    db.c.insert(docOrigin[0]);
    db.c.insert(docOrigin[1]);
    assert.eq(db.c.count(), 2, "collection count should be 2 at setup");

    var argv = ["import", "--file", "jstests/import/testdata/upsert1.json"];
    if (cmdArg) {
      argv.push(cmdArg);
    }
    argv = argv.concat(commonToolArgs);
    var ret = toolTest.runTool.apply(toolTest, argv);
    if (expectMode === "error") {
      return assert.neq(ret, 0);
    }
    assert.eq(ret, 0);
    assert.eq(db.c.count(), 2);

    var docs = [
      db.c.findOne({_id: "one"}),
      db.c.findOne({_id: "two"}),
    ];
    var docExpects = [];
    switch (expectMode) {
    case "insert":
      docExpects = docOrigin;
      break;
    case "upsert":
      docExpects = [
        {_id: "one", a: "unicorns", b: "zebras"},
        {_id: "two", a: "xxx", b: "yyy"},
      ];
      break;
    case "merge":
      docExpects = [
        {_id: "one", a: "unicorns", b: "zebras", x: "origin field"},
        {_id: "two", a: "xxx", b: "yyy", x: "origin field"},
      ];
      break;
    default:
      throw new Error();
    }
    assert.docEq(docs, docExpects);
  }

  // argument-1: expected behavior
  // argument-2: command argument for mongoimport

  testWithUpsertFields("error", "--mode=wrong");
  testWithUpsertFields("error", "--mode=insert");
  testWithUpsertFields("upsert", "");
  testWithUpsertFields("upsert", "--upsert"); // deprecated cmdArg
  testWithUpsertFields("upsert", "--mode=upsert");
  testWithUpsertFields("merge", "--mode=merge");

  testWithoutUpsertFields("error", "--mode=wrong");
  testWithoutUpsertFields("insert", "--mode=insert");
  testWithoutUpsertFields("insert", "");
  testWithoutUpsertFields("upsert", "--upsert"); // deprecated cmdArg
  testWithoutUpsertFields("upsert", "--mode=upsert");
  testWithoutUpsertFields("merge", "--mode=merge");

  toolTest.stop();
}());
