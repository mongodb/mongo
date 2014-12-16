if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

/*
 * If SSL is enabled in the config, this test starts mongod with SSL off and
 * tests that we get a sensible failure. Otherwise, it runs with --ssl and
 * asserts that we get a sensible failure.
 *
 * Note: this requires an SSL-enabled tool suite
 */

(function() {
  var toolTest = getToolTest('oplogDeprecatedFlagTest');
  var commonToolArgs = getCommonToolArguments();
  var sslOpts = [
    '--ssl',
    '--sslPEMKeyFile', 'jstests/libs/client.pem'
  ];

  if (toolTest.usesSSL) {
    var port = 26999;
    var mongod = startMongod('--auth', '--port', port,
      '--dbpath', MongoRunner.dataPath + 'oplogDeprecatedFlagTest2');

    /** Overwrite so toolTest.runTool doesn't append --host */
    toolTest.runTool = function() {
      arguments[0] = 'mongo' + arguments[0];
      return runMongoProgram.apply(null , arguments);
    };

    var args = ['oplog'].concat(commonToolArgs).concat('--host', '127.0.0.1',
      '--port', port, '--from', '127.0.0.1:' + toolTest.port);
    assert(toolTest.runTool.apply(toolTest, args) !== 0,
      'mongooplog should fail when --host does not support SSL but --ssl ' +
      'is specified');
  } else {
    var args = ['oplog', '--from', '127.0.0.1:' + toolTest.port].
      concat(commonToolArgs).concat(sslOpts);
    assert(toolTest.runTool.apply(toolTest, args) !== 0,
      'mongooplog should fail when --from doesnt support SSL but --ssl is ' +
      'specified');
  }

  toolTest.stop();
})();
