if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

load('jstests/common/check_version.js');

(function() {
  // skip tests requiring wiredTiger storage engine, since repair is not supported
  resetDbpath('dump');
  var toolTest = getToolTest('repairFlagTest');
  var commonToolArgs = getCommonToolArguments();
  var db = toolTest.db.getSiblingDB('foo');

  if (isAtLeastVersion(db.version(), '3.1.0')) {
    if (TestData && TestData.storageEngine != 'mmapv1') {
        jsTest.log("skipping test on "+db.version()+" when storage engine is not mmapv1");
        return;
    }
  } else {
    if (TestData && TestData.storageEngine === 'wiredTiger') {
        jsTest.log("skipping test on "+db.version()+" when storage engine is wiredTiger");
        return;
    }
  }

  db.dropDatabase();
  db.bar.insert({ x: 1 });

    // Running mongodump with '--repair' specified but no '--db' should fail
  var dumpArgs = ['dump', '--db', 'foo',
    '--repair'].
        concat(getDumpTarget()).
        concat(commonToolArgs);

  if (isAtLeastVersion(db.version(), '2.7.8') && !toolTest.isSharded) {
    // Should succeed on >= 2.7.8
    assert(toolTest.runTool.apply(toolTest, dumpArgs) === 0,
      'mongodump --repair should run successfully on mongodb >= 2.7.8 and ' +
      'not shared');
  } else {
    // Should fail fast on < 2.7.8
    assert(toolTest.runTool.apply(toolTest, dumpArgs) !== 0,
      'mongodump should exit with a non-zero status when --repair is ' +
      'specified on mongodb < 2.7.8 or when running against mongos');

    db.dropDatabase();

    var restoreArgs = ['restore'].
        concat(getRestoreTarget()).
        concat(commonToolArgs);
    assert.eq(toolTest.runTool.apply(toolTest, restoreArgs), 0,
      'mongorestore should succeed');
    assert.eq(0, db.bar.count());
    assert.eq(0, ls('dump/foo').length, 'dump directory should be empty, but it was not');
  }

  toolTest.stop();
})();
