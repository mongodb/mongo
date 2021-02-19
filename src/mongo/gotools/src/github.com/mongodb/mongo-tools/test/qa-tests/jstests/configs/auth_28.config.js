/* exported getToolTest */
var getToolTest;
var AUTH_USER = 'passwordIsTaco';
var AUTH_PASSWORD = 'Taco';

(function() {
  var TOOLS_TEST_CONFIG = {
    binVersion: '',
    auth: '',
  };

  getToolTest = function(name) {
    var toolTest = new ToolTest(name, TOOLS_TEST_CONFIG);
    var db = toolTest.startDB();

    db.getSiblingDB('admin').createUser({
      user: AUTH_USER,
      pwd: AUTH_PASSWORD,
      roles: ['__system'],
    });

    db.getSiblingDB('admin').auth(AUTH_USER, AUTH_PASSWORD);

    toolTest.authCommand = "db.getSiblingDB('admin').auth('" + AUTH_USER
      + "', '" + AUTH_PASSWORD + "');";

    return toolTest;
  };
}());

/* exported getCommonToolArguments */
var getCommonToolArguments = function() {
  return [
    '--username', AUTH_USER,
    '--password', AUTH_PASSWORD,
    '--authenticationDatabase', 'admin'
  ];
};
