(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var toolTest = getToolTest('OplogServerKOTest');
  var commonToolArgs = getCommonToolArguments();

  // Overwrite global db object for startParallelShell()
  db = toolTest.db.getSiblingDB('foo'); // eslint-disable-line no-native-reassign
  db.dropDatabase();

  var port = 26999;
  startMongod('--auth', '--port', port,
      '--dbpath', MongoRunner.dataPath + 'oplogServerKOTest2');

  var start = Date.now();

  // Insert into a fake oplog as fast as possible for 20 seconds
  while (Date.now() - start < 20000) {
    db.test.insert({breakfast: 'bacon'}, {w: 0});
  }

  // Run parallel shell that waits for mongooplog to start and kills the
  // server
  if (!toolTest.isReplicaSet || !toolTest.authCommand) {
    // shutdownServer() is flakey on replica sets because of localhost
    // exception, so do a stepdown instead
    print('Nothing to do: can only run server KO test with replica set + auth');
    return;
  }
  // Start a parallel shell to kill the server
  startParallelShell(
    'sleep(1000); ' +
    (toolTest.authCommand || '') +
    'print(\'Killing server!\');' +
    'db.getSiblingDB(\'admin\').shutdownServer({ force: true });');

  var args = ['oplog',
    '--from', '127.0.0.1:' + toolTest.port].concat(commonToolArgs);

  assert(toolTest.runTool.apply(toolTest, args) !== 0,
    'mongooplog should crash gracefully when remote server dies');

  var expected = 'error communicating with server';
  assert.strContains.soon(expected, rawMongoProgramOutput,
    'Should output sensible error message when host server dies');

  toolTest.stop();
}());
