if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
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
  var inserts = startParallelShell(
    'print(\'starting insert\'); ' +
    'for (var i = 1001; i < 2000; ++i) { ' +
    '  db.getSiblingDB(\'foo\').bar.insert({ x: i }); ' +
    '  sleep(1); ' +
    '}');

  // Give some time for inserts to actually start before dumping
  sleep(100);

  var countBeforeMongodump = db.bar.count();
  // Crash if parallel shell hasn't started inserting yet
  assert.gt(countBeforeMongodump, 1000);

  var dumpArgs = ['dump', '--oplog'].concat(commonToolArgs);
  toolTest.runTool.apply(toolTest, dumpArgs);

  // Wait for inserts to finish so we can then drop the database
  inserts();
  db.dropDatabase();
  assert.eq(0, db.bar.count());

  var restoreArgs = ['restore'].concat(commonToolArgs);
  toolTest.runTool.apply(toolTest, restoreArgs);
  assert.gte(db.bar.count(), countBeforeMongodump);
  assert.lt(db.bar.count(), 2000);

  toolTest.stop();
})();
