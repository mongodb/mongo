if (typeof getToolTest === 'undefined') {
  load('jstests/configs/replset_28.config.js');
}

(function() {
  resetDbpath('dump');
  var toolTest = getToolTest('oplogFlagTest');
  var commonToolArgs = getCommonToolArguments();

  // IMPORTANT: make sure global `db` object is equal to this db, because
  // startParallelShell gives you no way of overriding db object.
  db = toolTest.db.getSiblingDB('foo');

  db.dropDatabase();
  assert.eq(0, db.bar.count());
  for (var i = 0; i < 1000; ++i) {
    db.bar.insert({ x: i });
  }

  // Run parallel shell that inserts every millisecond
  var insertsShell = startParallelShell(
    'print(\'starting insert\'); ' +
    (toolTest.authCommand || '') +
    'for (var i = 1001; i < 10000; ++i) { ' +
    '  db.getSiblingDB(\'foo\').bar.insert({ x: i }); ' +
    '  sleep(1); ' +
    '}');

  // Give some time for inserts to actually start before dumping
  sleep(1000);

  var countBeforeMongodump = db.bar.count();
  // Crash if parallel shell hasn't started inserting yet
  assert.gt(countBeforeMongodump, 1000);

  var dumpArgs = ['dump', '--oplog'].
      concat(getDumpTarget()).
      concat(commonToolArgs);

  if (toolTest.isReplicaSet) {
    // If we're running in a replica set, --oplog should give a snapshot by
    // applying ops from the oplog
    assert.eq(toolTest.runTool.apply(toolTest, dumpArgs), 0,
      'mongodump --oplog should succeed');

    // Wait for inserts to finish so we can then drop the database
    insertsShell();
    db.dropDatabase();
    assert.eq(0, db.bar.count());

    var restoreArgs = ['restore'].
        concat(getRestoreTarget()).
        concat(commonToolArgs);
    assert.eq(toolTest.runTool.apply(toolTest, restoreArgs), 0,
      'mongorestore should succeed');
    assert.gte(db.bar.count(), countBeforeMongodump);
    assert.lt(db.bar.count(), 10000);
  } else {
    // If we're running against a standalone or sharded cluster, mongodump
    // --oplog should fail immediately, without dumping any data
    assert(toolTest.runTool.apply(toolTest, dumpArgs) !== 0,
      'mongodump --oplog should fail fast on sharded and standalone');

    // Wait for inserts to finish so we can then drop the database
    insertsShell();
    db.dropDatabase();
    assert.eq(0, db.bar.count());

    var restoreArgs = ['restore'].
        concat(getRestoreTarget()).
        concat(commonToolArgs);
    assert.eq(toolTest.runTool.apply(toolTest, restoreArgs), 0,
      'mongorestore should succeed');
    // Shouldn't have dumped any documents
    assert.eq(db.bar.count(), 0);
  }

  toolTest.stop();
})();
