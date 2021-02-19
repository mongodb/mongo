/* exported getToolTest */
var getToolTest;

(function() {
  var AUTH_USER = 'mockuser@10GEN.ME';

  var TOOLS_TEST_CONFIG = {
    binVersion: '',
    config: 'jstests/configs/kerberos.config.yml',
  };

  getToolTest = function(name) {
    var toolTest = new ToolTest(name, TOOLS_TEST_CONFIG);
    var db = toolTest.startDB();

    db.getSiblingDB('$external').createUser({
      user: AUTH_USER,
      roles: [{role: '__system', db: 'admin'}],
    });

    db.getSiblingDB('$external').auth({user: AUTH_USER, mechanism: 'GSSAPI', serviceName: 'mockservice', serviceHostname: 'kdc.10gen.me'});

    toolTest.authCommand = "db.getSiblingDB('$external').auth({ user: '"
      + AUTH_USER + "', mechanism: 'GSSAPI', serviceName: 'mockservice', serviceHostname: 'kdc.10gen.me' });";

    return toolTest;
  };
}());

/* exported getCommonToolArguments */
var getCommonToolArguments = function() {
  return [
    '--username', 'mockuser@10GEN.ME',
    '--authenticationDatabase', '$external',
    '--authenticationMechanism', 'GSSAPI',
    '--gssapiServiceName', 'mockservice',
    '--gssapiHostName', 'kdc.10gen.me'
  ];
};
