if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

(function() {
  resetDbpath('dump');
  var toolTest = getToolTest('DBDroppedTest');
  var commonToolArgs = getCommonToolArguments();

  // IMPORTANT: make sure global `db` object is equal to this db, because
  // startParallelShell gives you no way of overriding db object.
  db = toolTest.db.getSiblingDB('foo');

  db.dropDatabase();
  assert.eq(0, db.bar.count());

  for (var i = 0; i < 1000; ++i) {
    db.bar.insert({ x: i });
  }

  // Run parallel shell that waits for mongodump to start and then
  // drops the underlying database
  var insertsShell = startParallelShell(
    'sleep(1000); ' +
    (toolTest.authCommand || '') +
    'db.getSiblingDB(\'foo\').dropDatabase();');

  var dumpArgs = ['dump', '--db', 'foo', '--collection', 'bar',
    '--query', '{ $where: "sleep(10); return true;" }'].concat(commonToolArgs);

  assert(toolTest.runTool.apply(toolTest, dumpArgs) !== 0,
    'mongodump should crash gracefully when database is dropped');

  var output = rawMongoProgramOutput();
  var expectedError = 'error reading from db: Exec error: PlanExecutor killed';
  assert(output.indexOf(expectedError) !== -1,
    'mongodump crash should output the correct error message');

  toolTest.stop();
})();
