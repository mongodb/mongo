/*
 * Tests that the informational flags --version and --help give reasonable
 * output.
 */
(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var toolTest = getToolTest('oplogInformationalFlagTest');
  var commonToolArgs = getCommonToolArguments();

  var verifyFlagOutput = function(flag, expected) {
    var args = ['oplog'].concat(commonToolArgs).concat(flag);
    assert.eq(toolTest.runTool.apply(toolTest, args), 0,
      'mongooplog should succeed with ' + flag);

    assert.strContains.soon(expected, rawMongoProgramOutput,
      'mongooplog ' + flag + " should produce output that contains '" +
      expected + "'");
  };

  verifyFlagOutput('--help', 'Usage:');

  toolTest.stop();
}());
