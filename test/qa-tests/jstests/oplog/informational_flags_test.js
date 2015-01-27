if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

/*
 * Tests that the informational flags --version and --help give reasonable
 * output.
 */

(function() {
  var toolTest = getToolTest('oplogInformationalFlagTest');
  var commonToolArgs = getCommonToolArguments();

  var verifyFlagOutput = function(flag, expected) {
    var args = ['oplog'].concat(commonToolArgs).concat(flag);
    assert.eq(toolTest.runTool.apply(toolTest, args), 0,
      'mongooplog should succeed with ' + flag);

    var output = rawMongoProgramOutput();
    
    assert(output.indexOf(expected) !== -1,
      'mongooplog ' + flag + ' should produce output that contains "' +
      expected + "'");
  };

  verifyFlagOutput('--help', 'Usage:');

  toolTest.stop();
})();
