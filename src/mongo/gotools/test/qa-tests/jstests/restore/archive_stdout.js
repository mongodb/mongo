(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  var toolTest = getToolTest('archive_stdout');
  var baseArgs = getCommonToolArguments();
  baseArgs = baseArgs.concat('--port', toolTest.port);

  if (toolTest.useSSL) {
    baseArgs = baseArgs.concat([
      '--ssl',
      '--sslPEMKeyFile', 'jstests/libs/server.pem',
      '--sslCAFile', 'jstests/libs/ca.pem',
      '--sslAllowInvalidHostnames']);
  }
  if (dump_targets === 'gzip') {
    baseArgs = baseArgs.concat('--gzip');
  }
  var dumpArgs = ['mongodump', '--archive'].concat(baseArgs);
  var restoreArgs = ['mongorestore', '--archive', '--drop'].concat(baseArgs);

  dumpArgs[0] = 'PATH=.:$PATH ' + dumpArgs[0];
  restoreArgs[0] = 'PATH=.:$PATH ' + restoreArgs[0];
  if (_isWindows()) {
    dumpArgs[0] += '.exe';
    restoreArgs[0] += '.exe';
  }

  var testDb = toolTest.db;
  testDb.dropDatabase();
  var fooData = [];
  var barData = [];
  for (var i = 0; i < 500; i++) {
    fooData.push({i: i});
    barData.push({i: i*5});
  }
  testDb.foo.insertMany(fooData);
  testDb.bar.insertMany(barData);
  assert.eq(500, testDb.foo.count(), 'foo should have our test documents');
  assert.eq(500, testDb.bar.count(), 'bar should have our test documents');

  var ret = runProgram('bash', '-c', dumpArgs.concat('|', restoreArgs).join(' '));
  assert.eq(0, ret, "bash execution should succeed");

  for (i = 0; i < 500; i++) {
    assert.eq(1, testDb.foo.find({i: i}).count(), 'document #'+i+' not in foo');
    assert.eq(1, testDb.bar.find({i: i*5}).count(), 'document #'+i+' not in bar');
  }
  assert.eq(500, testDb.foo.count(), 'foo should have our test documents');
  assert.eq(500, testDb.bar.count(), 'bar should have our test documents');

  testDb.dropDatabase();
  toolTest.stop();
}());
