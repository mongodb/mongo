if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

(function() {
  resetDbpath('dump');
  var toolTest = getToolTest('ServerKOTest');
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
  // brings the server down.
  if (toolTest.isReplicaSet && !toolTest.authCommand) {
    // shutdownServer() is flakey on replica sets because of localhost
    // exception, so do a stepdown instead
    return assert(false, 'Can\'t run shutdownServer() on replica set ' +
      'without auth!');
  } else {
    // On sharded and standalone, kill the server
    startParallelShell(
      'sleep(500); ' +
      (toolTest.authCommand || '') +
      'db.getSiblingDB(\'admin\').shutdownServer({ force: true });');
  }

  var dumpArgs = ['dump', '--db', 'foo', '--collection', 'bar',
    '--query', '{ $where: "sleep(25); return true;" }'].
        concat(getDumpTarget()).
        concat(commonToolArgs);

  assert(toolTest.runTool.apply(toolTest, dumpArgs) !== 0,
    'mongodump should crash gracefully when remote server dies');

  var output = rawMongoProgramOutput();
  var expectedError1 = 'error reading from db';
  var expectedError2 = 'error reading collection';
  assert( output.indexOf(expectedError1) !== -1 || output.indexOf(expectedError2) !== -1 ,
    'mongodump crash should output the correct error message');

  toolTest.stop();
})();
