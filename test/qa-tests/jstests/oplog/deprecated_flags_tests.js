if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

(function() {
  var toolTest = getToolTest('oplogDeprecatedFlagTest');
  var commonToolArgs = getCommonToolArguments();
  var expectedError = 'error parsing command line: --dbpath and related ' +
    'flags are not supported in 2.8 tools.';

  var verifyFlagFails = function(flag) {
    var args = ['oplog'].concat(commonToolArgs).concat(flag);
    assert(toolTest.runTool.apply(toolTest, args) !== 0,
      'mongooplog should fail when --dbpath specified');

    var output = rawMongoProgramOutput();
    
    assert(output.indexOf(expectedError) !== -1,
      'mongooplog should output the correct error message');
  };

  verifyFlagFails('--dbpath');
  verifyFlagFails('--directoryperdb');
  verifyFlagFails('--journal');

  toolTest.stop();
})();
