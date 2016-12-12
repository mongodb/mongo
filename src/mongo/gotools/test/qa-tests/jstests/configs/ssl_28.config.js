/* exported getToolTest */
var getToolTest;

(function() {
  var TOOLS_TEST_CONFIG = {
    binVersion: '',
    sslMode: 'requireSSL',
    sslPEMKeyFile: 'jstests/libs/server.pem',
    sslCAFile: 'jstests/libs/ca.pem',
  };

  getToolTest = function(name) {
    var toolTest = new ToolTest(name, TOOLS_TEST_CONFIG);
    toolTest.startDB();
    toolTest.usesSSL = true;
    return toolTest;
  };
}());

/* exported getCommonToolArguments */
var getCommonToolArguments = function() {
  return [
    '--ssl',
    '--sslPEMKeyFile', 'jstests/libs/client.pem'
  ];
};
