/*
 * Tests that we provide helpful output when user tries to use flags that were
 * deprecated in 2.7.x
 */
(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var toolTest = getToolTest('oplogDeprecatedFlagTest');
  var commonToolArgs = getCommonToolArguments();
  var expectedError = 'error parsing command line options: --dbpath and related ' +
    'flags are not supported in 3.0 tools.';

  var verifyFlagFails = function(flag) {
    var args = ['oplog'].concat(commonToolArgs).concat(flag);
    assert(toolTest.runTool.apply(toolTest, args) !== 0,
      'mongooplog should fail when --dbpath specified');

    assert.strContains.soon(expectedError, rawMongoProgramOutput,
        'mongooplog should output the correct error message');
  };

  verifyFlagFails('--dbpath');
  verifyFlagFails('--directoryperdb');
  verifyFlagFails('--journal');

  toolTest.stop();
}());
