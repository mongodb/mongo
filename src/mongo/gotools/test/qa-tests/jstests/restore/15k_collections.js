// this tests that we can restore a large number of collections, resolving
// an issue raised by TOOLS-1088
// @tags: [requires_many_files, requires_large_ram]
(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var toolTest = getToolTest('15k_collections');
  var commonToolArgs = getCommonToolArguments();

  var dbOne = toolTest.db.getSiblingDB('dbOne');

  for (var i=0; i<=15000; i++) {
    collName = "Coll" + i;
    dbOne.createCollection(collName);
  }

  // dump it
  var dumpTarget = '15k_collections_dump';
  resetDbpath(dumpTarget);
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // drop the database so it's empty
  dbOne.dropDatabase();

  // restore it
  ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret, "restore to empty DB should have returned successfully");

  // success
  toolTest.stop();
}());
