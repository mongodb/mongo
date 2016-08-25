(function() {
  jsTest.log('Testing running import with upserts');
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var toolTest = getToolTest('import_dupes');
  var db1 = toolTest.db;
  var commonToolArgs = getCommonToolArguments();

  var db = db1.getSiblingDB("dupetest");
  db.dropDatabase();

  // Verify that --upsert with --upsertFields works by applying update w/ query on the fields
  db.c.insert({_id: 1234, b: "000000", c: 222});
  assert.eq(db.c.count(), 1, "collection count should be 1 at setup");
  var ret = toolTest.runTool.apply(toolTest, ["import", "--file",
      "jstests/import/testdata/dupes.json",
      "--db", db.getName(),
      "--collection", db.c.getName(),
      "--stopOnError"]
    .concat(commonToolArgs));

  assert.neq(ret, 0,
      "duplicate key with --stopOnError should return nonzero exit code");

  // drop it, try again without stop on error
  db.c.drop();
  db.c.insert({_id: 1234, b: "000000", c: 222});
  ret = toolTest.runTool.apply(toolTest, ["import", "--file",
      "jstests/import/testdata/dupes.json",
      "--db", db.getName(),
      "--collection", db.c.getName()]
    .concat(commonToolArgs));
  assert.eq(ret, 0,
      "duplicate key without --stopOnError should return zero exit code");
  assert.docEq(db.c.findOne({_id: 1234}), {_id: 1234, b: "000000", c: 222});

  toolTest.stop();
}());
