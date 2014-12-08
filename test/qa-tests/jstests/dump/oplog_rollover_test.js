if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

(function() {
  resetDbpath('dump');
  var toolTest = getToolTest('oplogRolloverTest');
  var commonToolArgs = getCommonToolArguments();

  if (!toolTest.isReplicaSet) {
    print('Nothing to do for testing oplog rollover without a replica set!');
    return assert(true);
  }

  // IMPORTANT: make sure global `db` object is equal to this db, because
  // startParallelShell gives you no way of overriding db object.
  db = toolTest.db.getSiblingDB('foo');

  db.dropDatabase();
  assert.eq(0, db.bar.count());

  // Run parallel shell that inserts large documents as fast as possible. Each
  // document should be > 4MB, and thus (almost) every write should overflow
  // a 5MB oplog, which is the oplog size that these tests are designed for.
  var insertsShell = startParallelShell(
    'print(\'starting insert\'); ' +
    (toolTest.authCommand || '') +
    'var longString = \'\'; ' +
    'while (longString.length < 4 * 1024 * 1024) { longString += \'bacon\'; } ' +
    'for (var i = 0; i < 1000; ++i) { ' +
    '  db.getSiblingDB(\'foo\').bar.insert({ x: longString }); ' +
    '}');

  // Give some time for inserts to actually start before dumping, we only need
  // one document to go in and 0.5 seconds should be enough unless your
  // scheduler is wonky or your HDD is really slow.
  sleep(500);

  var countBeforeMongodump = db.bar.count();
  // Crash if parallel shell hasn't started inserting yet
  assert.gt(countBeforeMongodump, 0, 'Didn\'t successfully start inserting ' +
    'large documents before mongodump');

  var dumpArgs = ['dump', '--oplog'].concat(commonToolArgs);

  assert(toolTest.runTool.apply(toolTest, dumpArgs) !== 0,
    'mongodump --oplog should crash sensibly on oplog rollover');

  var output = rawMongoProgramOutput();
  var expectedError = 'oplog overflow: mongodump was unable to capture all ' +
    'new oplog entries during execution';
  assert(output.indexOf(expectedError) !== -1,
    'mongodump --oplog failure should output the correct error message');

  toolTest.stop();
})();
