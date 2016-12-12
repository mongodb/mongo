// topology_helper.js; contains utility functions to run tests

// auth related variables
var authUser = 'user';
var authPassword = 'password';
var authArgs = [
  '--authenticationDatabase', 'admin',
  '--authenticationMechanism', 'SCRAM-SHA-1',
  '-u', authUser,
  '-p', authPassword
];
var keyFile = 'jstests/libs/key1';

// topology startup settings
var auth = {
  name: 'auth',
  args: authArgs,
};

var plain = {
  name: 'plain',
  args: [],
};

/* exported passthroughs */
// passthroughs while running all tests
var passthroughs = [plain, auth];

/* helper functions */

// runAuthSetup creates a user with root role on the admin database
var runAuthSetup = function(topology) {
  jsTest.log('Running auth setup');

  var conn = topology.connection();
  var db = conn.getDB('test');

  db.getSiblingDB('admin').createUser({
    user: authUser,
    pwd: authPassword,
    roles: ['root'],
  });

  assert.eq(db.getSiblingDB('admin').auth(authUser, authPassword), 1, 'authentication failed');
};

// buildStartupArgs constructs the proper object to be passed as arguments in
// starting mongod
var buildStartupArgs = function(passthrough) {
  var startupArgs = {};
  if (passthrough.name === auth.name) {
    startupArgs.auth = '';
    startupArgs.keyFile = keyFile;
  }
  return startupArgs;
};

// requiresAuth returns a boolean indicating if the passthrough requires authentication
var requiresAuth = function(passthrough) {
  return passthrough.name === auth.name;
};

/* standalone topology */
/* exported standaloneTopology */
var standaloneTopology = {
  init: function(passthrough) {
    jsTest.log('Using standalone topology');

    passthrough = passthrough || [];
    var startupArgs = buildStartupArgs(passthrough);
    startupArgs.port = allocatePorts(1)[0];
    this.conn = MongoRunner.runMongod(startupArgs);

    // set up the auth user if needed
    if (requiresAuth(passthrough)) {
      runAuthSetup(this);
    }
    return this;
  },
  connection: function() {
    return this.conn;
  },
  stop: function() {
    MongoRunner.stopMongod(this.conn);
  },
};


/* replica set topology */
/* exported replicaSetTopology */
var replicaSetTopology = {
  init: function(passthrough) {
    jsTest.log('Using replica set topology');

    passthrough = passthrough || [];
    var startupArgs = buildStartupArgs(passthrough);
    startupArgs.name = testName;
    startupArgs.nodes = 2;
    this.replTest = new ReplSetTest(startupArgs);

    // start the replica set
    this.replTest.startSet();
    jsTest.log('Started replica set');

    // initiate the replica set with a default config
    this.replTest.initiate();
    jsTest.log('Initiated replica set');

    // block till the set is fully operational
    this.replTest.awaitSecondaryNodes();
    jsTest.log('Replica set fully operational');

    // set up the auth user if needed
    if (requiresAuth(passthrough)) {
      runAuthSetup(this);
    }
    return this;
  },
  connection: function() {
    return this.replTest.getPrimary();
  },
  stop: function() {
    this.replTest.stopSet();
  },
};


/* sharded cluster topology */
/* exported shardedClusterTopology */
var shardedClusterTopology = {
  init: function(passthrough) {
    jsTest.log('Using sharded cluster topology');

    passthrough = passthrough || [];
    var other = buildStartupArgs(passthrough);
    var startupArgs = {};
    startupArgs.name = testName;
    startupArgs.mongos = 1;
    startupArgs.shards = 1;

    // set up the auth user if needed
    if (requiresAuth(passthrough)) {
      startupArgs.keyFile = keyFile;
      startupArgs.other = {
        shardOptions: other,
      };
      this.shardingTest = new ShardingTest(startupArgs);
      runAuthSetup(this);
    } else {
      startupArgs.other = {
        shardOptions: other,
      };
      this.shardingTest = new ShardingTest(startupArgs);
    }
    return this;
  },
  connection: function() {
    return this.shardingTest.s;
  },
  stop: function() {
    this.shardingTest.stop();
  },
};

