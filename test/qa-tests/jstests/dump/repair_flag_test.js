if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

load('jstests/common/check_version.js');

(function() {
  resetDbpath('dump');
  var toolTest = getToolTest('repairFlagTest');
  var commonToolArgs = getCommonToolArguments();
  var db = toolTest.db.getSiblingDB('foo');

  db.dropDatabase();

    // Running mongodump with '--repair' specified but no '--db' should fail
  var dumpArgs = ['dump', '--db', 'foo',
    '--repair'].concat(commonToolArgs);
  if (isAtLeastVersion(db.version(), '2.7.8')) {
    assert(toolTest.runTool.apply(toolTest, dumpArgs) === 0,
      'mongodump --repair should run successfully on mongodb >= 2.7.8');
  } else {
    assert(toolTest.runTool.apply(toolTest, dumpArgs) !== 0,
      'mongodump should exit with a non-zero status when --repair is ' +
      'specified on mongodb < 2.7.8');
  }

  toolTest.stop();
})();
