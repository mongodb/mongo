// runWCTest executes a tool against a number of configurations. A given replica set will have nodes prevented
// from replicating and the tool should either pass or fail based on the supplied write concern. As a final test,
// the tools is run with w:3, and waits for all three nodes to come back online, simulating a slowly-replicated write.
var runWCTest = function runWCTest(progName, rs, toolTest, testWriteConcern, testProgramNoConnect, testSetupFunction) {
  jsTest.log("testing that "+progName+" deals with write concern");

  function windowsEscape(json) {
    if (_isWindows()) {
      json = '"' + json.replace(/"/g, '\\"') + '"';
    }
    return json;
  }

  function stopSync(nodes) {
    jsTest.log("stopping "+nodes.length+" nodes");
    for (var i = 0; i < nodes.length; i++) {
      nodes[i].runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});
    }
    sleep(2000);
  }

  function startSync(nodes) {
    jsTest.log("starting "+nodes.length+" nodes");
    for (var i = 0; i < nodes.length; i++) {
      nodes[i].runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'});
    }
  }

  function loggedTestSetup() {
    if (testSetupFunction) {
      jsTest.log("running test setup");
      testSetupFunction();
    }
  }

  if (!testSetupFunction) {
    testSetupFunction = function() {};
  }

  // grab the two secondary nodes
  var masterPort = rs.getPrimary().port;
  var members = [];
  var stopped = [];
  var ports = [];
  for (var i = 0; i < rs.nodes.length; i++) {
    if (rs.nodes[i].port !== masterPort) {
      members.push(rs.nodes[i].getDB("admin"));
      ports.push(rs.nodes[i].port);
    }
  }
  var member1 = members[0];
  var member2 = members[1];

  loggedTestSetup();
  testWriteConcern(0, [], progName+" without write concern to a fully functioning repl-set should succeed");

  loggedTestSetup();
  testWriteConcern(0, ['--writeConcern=majority'], progName+" with majority to a fully functioning repl-set should succeed");

  loggedTestSetup();
  testWriteConcern(0, ['--writeConcern={w:1,wtimeout:10000}'], progName+" with w:1,timeout:10000 to a fully functioning repl-set should succeed");

  loggedTestSetup();
  testWriteConcern(0, ['--writeConcern={w:2,wtimeout:10000}'], progName+" with w:2,timeout:10000 to a fully functioning repl-set should succeed");

  jsTest.log("stopping node on port " + ports[0] + " from doing any further syncing");
  stopped.push(member1);

  loggedTestSetup();
  stopSync(stopped);
  testWriteConcern(0, ['--writeConcern={w:1,wtimeout:10000}'], progName+" with w:1,timeout:10000 repl-set with 2 working nodes should succeed");
  startSync(stopped);

  loggedTestSetup();
  stopSync(stopped);
  testWriteConcern(0, ['--writeConcern={w:2,wtimeout:10000}'], progName+" with w:2,timeout:10000 repl-set with 2 working nodes should succeed");
  startSync(stopped);

  loggedTestSetup();
  stopSync(stopped);
  testWriteConcern(0, ['--writeConcern=majority'], progName+" with majority with two working nodes should succeed");
  startSync(stopped);

  loggedTestSetup();
  stopSync(stopped);
  testWriteConcern(1, ['--writeConcern={w:3,wtimeout:2000}'], progName+" with w:3,timeout:2000 repl-set with two working nodes should fail");
  startSync(stopped);

  jsTest.log("stopping second node on port " + ports[1] + " from doing any further syncing");
  stopped.push(member2);

  loggedTestSetup();
  stopSync(stopped);
  testWriteConcern(1, [windowsEscape('--writeConcern={w:"majority",wtimeout:2000}')], progName+" with majority with one working node should fail");
  startSync(stopped);

  loggedTestSetup();
  stopSync(stopped);
  testWriteConcern(1, ['--writeConcern={w:2,wtimeout:10000}'], progName+" with w:2,timeout:10000 with one working node should fail");
  startSync(stopped);

  loggedTestSetup();
  stopSync(stopped);
  testWriteConcern(0, ['--writeConcern={w:1,wtimeout:10000}'], progName+" with w:1,timeout:10000 repl-set with one working nodes should succeed");
  startSync(stopped);

  loggedTestSetup();
  stopSync(stopped);
  jsTest.log(progName+" with w:3 concern and no working member and no timeout waits until member are available");
  pid = testProgramNoConnect();
  sleep(2000);
  assert(checkProgram(pid), progName+" with w:3 and no working members should not have finished");
  startSync(stopped);

  jsTest.log("waiting for "+progName+" to finish");
  ret = waitProgram(pid);
  assert.eq(0, ret, progName+" with w:3 should succeed once enough members start working");
};
