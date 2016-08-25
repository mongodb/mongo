(function() {
  jsTest.log('Testing running import with upserts');

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  jsTest.log('Testing running import with bad command line options');

  var toolTest = getToolTest('import');
  var db1 = toolTest.db;
  var commonToolArgs = getCommonToolArguments();

  var db = db1.getSiblingDB("upserttest");
  db.dropDatabase();

  // Verify that --upsert with --upsertFields works by applying update w/ query on the fields
  db.c.insert({a: 1234, b: "000000", c: 222});
  db.c.insert({a: 4567, b: "111111", c: 333});
  assert.eq(db.c.count(), 2, "collection count should be 2 at setup");
  var ret = toolTest.runTool.apply(toolTest, ["import",
      "--file", "jstests/import/testdata/upsert2.json",
      "--upsert",
      "--upsertFields", "a,c",
      "--db", db.getName(),
      "--collection", db.c.getName()]
    .concat(commonToolArgs));

  var doc1 = db.c.findOne({a: 1234});
  delete doc1["_id"];
  assert.docEq(doc1, {a: 1234, b: "blah", c: 222});

  var doc2_1 = db.c.findOne({a: 4567, c: 333});
  var doc2_2 = db.c.findOne({a: 4567, c: 222});
  delete doc2_1["_id"];
  delete doc2_2["_id"];

  assert.docEq(doc2_1, {a: 4567, b: "yyy", c: 333});
  assert.docEq(doc2_2, {a: 4567, b: "asdf", c: 222});


  // Verify that --upsert without --upsertFields works by applying the update using _id
  db.c.drop();
  db.c.insert({_id: "one", a: "original value"});
  db.c.insert({_id: "two", a: "original value 2"});
  assert.eq(db.c.count(), 2, "collection count should be 2 at setup");

  toolTest.runTool.apply(toolTest, ["import",
      "--file", "jstests/import/testdata/upsert1.json",
      "--upsert",
      "--db", db.getName(),
      "--collection", db.c.getName()]
    .concat(commonToolArgs));

  // check that the upsert got applied
  assert.eq(ret, 0);
  assert.eq(db.c.count(), 2);

  assert.docEq(db.c.findOne({_id: "one"}), {_id: "one", a: "unicorns", b: "zebras"});

  toolTest.stop();
}());
