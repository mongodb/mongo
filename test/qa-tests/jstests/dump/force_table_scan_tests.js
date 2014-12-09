if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

(function() {
  resetDbpath('dump');
  var toolTest = getToolTest('forceTableScanTest');
  var commonToolArgs = getCommonToolArguments();

  // IMPORTANT: make sure global `db` object is equal to this db, because
  // startParallelShell gives you no way of overriding db object.
  db = toolTest.db.getSiblingDB('foo');

  db.dropDatabase();
  assert.eq(0, db.bar.count());

  // Run parallel shell that inserts every millisecond
  var insertsShell = startParallelShell(
    'print(\'starting insert\'); ' +
    (toolTest.authCommand || '') +
    'for (var i = 0; i < 10000; ++i) { ' +
    '  db.getSiblingDB(\'foo\').bar.insert({ x: i }); ' +
    '  sleep(1); ' +
    '}');

  // Give some time for inserts to actually start before dumping
  sleep(1000);

  var countBeforeMongodump = db.bar.count();
  // Crash if parallel shell hasn't started inserting yet
  assert.gt(countBeforeMongodump, 0);

  var dumpArgs = ['dump', '--forceTableScan'].concat(commonToolArgs);
  assert.eq(toolTest.runTool.apply(toolTest, dumpArgs), 0,
    'mongodump --forceTableScan should succeed');

  // Wait for inserts to finish so we can then drop the database
  insertsShell();
  db.dropDatabase();
  assert.eq(0, db.bar.count());

  // --batchSize is necessary because config servers don't allow
  // batch writes, so if you've dumped the config DB you should
  // be careful to set this.
  var restoreArgs = ['restore', '--batchSize', '1',
    '--drop'].concat(commonToolArgs);
  assert.eq(toolTest.runTool.apply(toolTest, restoreArgs), 0,
    'mongorestore should succeed');
  assert.gte(db.bar.count(), countBeforeMongodump);
  assert.lt(db.bar.count(), 1000);

  toolTest.stop();
})();
