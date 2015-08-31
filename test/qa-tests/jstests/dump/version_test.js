if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

(function() {
  resetDbpath('dump');
  var toolTest = getToolTest('versionTest');
  var commonToolArgs = getCommonToolArguments();
  var db = toolTest.db.getSiblingDB('foo');

  db.dropDatabase();
  assert.eq(0, db.bar.count());

  db.bar.insert({ x: 1 });

  var dumpArgs = ['dump', '--db', 'foo', '--archive=foo.archive'].
      concat(commonToolArgs);
  assert.eq(toolTest.runTool.apply(toolTest, dumpArgs), 0,
    'mongodump should succeed');

  db.dropDatabase();

  clearRawMongoProgramOutput();

  var restoreArgs = ['restore', '--archive=foo.archive', '-vvv'].
      concat(commonToolArgs);
  assert.eq(toolTest.runTool.apply(toolTest, restoreArgs), 0,
    'mongorestore should succeed');

  var out = rawMongoProgramOutput();

  assert(/archive format version "\S+"/.test(out),"format version found");
  assert(/archive server version "\S+"/.test(out),"server version found");
  assert(/archive tool version "\S+"/.test(out),"tool version found");

  toolTest.stop();
})();
