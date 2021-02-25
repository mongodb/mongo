/* exported getToolTest */
var getToolTest;

var AUTH_USER = 'passwordIsTaco';
var AUTH_PASSWORD = 'Taco';

(function() {
  getToolTest = function(name) {
    var toolTest = new ToolTest(name, null);

    var replTest = new ReplSetTest({
      name: 'tool_replset',
      nodes: 3,
      oplogSize: 5,
      auth: '',
      keyFile: 'jstests/libs/key1',
    });

    nodes = replTest.startSet();
    replTest.initiate();
    var master = replTest.getPrimary();

    toolTest.m = master;
    toolTest.db = master.getDB(name);
    toolTest.port = replTest.getPort(master);

    var db = toolTest.db;
    db.getSiblingDB('admin').createUser({
      user: AUTH_USER,
      pwd: AUTH_PASSWORD,
      roles: ['__system'],
    });

    db.getSiblingDB('admin').auth(AUTH_USER, AUTH_PASSWORD);

    var oldStop = toolTest.stop;
    toolTest.stop = function() {
      replTest.stopSet();
      oldStop.apply(toolTest, arguments);
    };

    toolTest.authCommand = 'db.getSiblingDB(\'admin\').auth(\'' +
      AUTH_USER + '\', \'' + AUTH_PASSWORD + '\');';

    toolTest.isReplicaSet = true;

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
