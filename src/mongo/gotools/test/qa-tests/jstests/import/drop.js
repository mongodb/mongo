(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  jsTest.log('Testing running import with bad command line options');

  var toolTest = getToolTest('import_writes');
  var db = toolTest.db.getSiblingDB("droptest");
  var commonToolArgs = getCommonToolArguments();

  // Verify that --drop works.
  // put a test doc in the collection, run import with --drop,
  // make sure that the inserted doc is gone and only the imported
  // docs are left.
  db.c.insert({x: 1});
  assert.eq(db.c.count(), 1, "collection count should be 1 at setup");
  var ret = toolTest.runTool.apply(toolTest, ["import",
      "--file", "jstests/import/testdata/csv_header.csv",
      "--type=csv",
      "--db", db.getName(),
      "--collection", db.c.getName(),
      "--headerline",
      "--drop"]
    .concat(commonToolArgs));

  // test csv file contains 3 docs and collection should have been dropped, so the doc we inserted
  // should be gone and only the docs from the test file should be in the collection.
  assert.eq(ret, 0);
  assert.eq(db.c.count(), 3);
  assert.eq(db.c.count({x: 1}), 0);

  // --drop on a non-existent collection should not cause error
  db.c.drop();
  ret = toolTest.runTool.apply(toolTest, ["import",
      "--file", "jstests/import/testdata/csv_header.csv",
      "--type=csv",
      "--db", db.getName(),
      "--collection", db.c.getName(),
      "--headerline",
      "--drop"]
    .concat(commonToolArgs));
  assert.eq(ret, 0);
  assert.eq(db.c.count(), 3);

  toolTest.stop();
}());
