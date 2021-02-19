load("jstests/configs/standard_dump_targets.config.js");

/* exported getToolTest */
var getToolTest;

(function() {
  var TOOLS_TEST_CONFIG = {
    binVersion: '',
  };

  getToolTest = function(name) {
    var toolTest = new ToolTest(name, TOOLS_TEST_CONFIG);
    toolTest.startDB();
    return toolTest;
  };
}());

/* exported getCommonToolArguments */
var getCommonToolArguments = function() {
  return [];
};
