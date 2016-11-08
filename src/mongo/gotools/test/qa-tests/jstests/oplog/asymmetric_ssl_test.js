/*
 * If SSL is enabled in the config, this test starts mongod with SSL off and
 * tests that we get a sensible failure. Otherwise, it runs with --ssl and
 * asserts that we get a sensible failure.
 *
 * Note: this requires an SSL-enabled tool suite
 */
(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var toolTest = getToolTest('oplogAsymmetricSSLTest');
  var commonToolArgs = getCommonToolArguments();
  var sslOpts = [
    '--ssl',
    '--sslPEMKeyFile', 'jstests/libs/client.pem'
  ];

  if (toolTest.useSSL) {
    var port = allocatePort();

    // this mongod is actually started with SSL flags because of `useSSL`
    startMongod('--auth', '--port', port,
        '--dbpath', MongoRunner.dataPath + 'oplogAsymmetricSSLTest2');

    var args = ['mongooplog'].concat(commonToolArgs).concat(
      '--from', '127.0.0.1:' + toolTest.port, '--host', '127.0.0.1', '--port', port);

    // mongooplog run without SSL against a destination server started with SSL should fail
    jsTest.log("Running mongooplog without SSL against mongod with SSL");
    assert.neq(runProgram.apply(this, args), 0,
      'mongooplog should fail when run without SSL flags against destination host (--host) ' +
      'started with SSL');
  } else {
    // toolTest.runTool will add the underlying --host argument for the mongod started without SSL
    args = ['oplog'].concat(commonToolArgs).concat(sslOpts).concat(
      '--from', '127.0.0.1:' + toolTest.port);

    // mongooplog run with SSL against a destination server not started with SSL should fail
    jsTest.log("Running mongooplog with SSL against mongod without SSL");
    assert.neq(toolTest.runTool.apply(toolTest, args), 0,
      'mongooplog should fail when run with SSL flags against destination host (--host) ' +
      'not started with SSL');
  }

  toolTest.stop();
}());
