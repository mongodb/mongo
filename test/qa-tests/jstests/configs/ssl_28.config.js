var getToolTest;

(function() {
  var TOOLS_TEST_CONFIG = {
    binVersion: '',
    sslMode: 'requireSSL',
    sslPEMKeyFile: '../../libs/server.pem',
    sslCAFile: '../../libs/ca.pem'
  };

  getToolTest = function(name) {
    var toolTest = new ToolTest(name, TOOLS_TEST_CONFIG);
    toolTest.startDB();
    return toolTest;
  };
})();

var getCommonToolArguments = function() {
  return [
    '--ssl',
    '--sslPEMKeyFile', '../../libs/client.pem'
  ];
};
