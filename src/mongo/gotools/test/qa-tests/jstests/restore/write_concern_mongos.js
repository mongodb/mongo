(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  var toolTest = new ToolTest('write_concern', null);
  var st = new ShardingTest({
    shards: {
      rs0: {
        nodes: 3,
        useHostName: true,
        settings: {chainingAllowed: false},
      },
    },
    mongos: 1,
    config: 1,
    configReplSetTestOptions: {
      settings: {chainingAllowed: false},
    },
  });
  var rs = st.rs0;
  rs.awaitReplication();
  toolTest.port = st.s.port;
  var commonToolArgs = getCommonToolArguments();
  var dbOne = st.s.getDB("dbOne");

  // create a test collection
  for (var i=0; i<=100; i++) {
    dbOne.test.insert({_id: i, x: i*i});
  }
  rs.awaitReplication();

  // dump the data that we'll
  var dumpTarget = 'write_concern_mongos_dump';
  resetDbpath(dumpTarget);
  var ret = toolTest.runTool.apply(toolTest, ['dump', '-d', 'dbOne']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  function writeConcernTestFunc(exitCode, writeConcern, name) {
    jsTest.log(name);
    var ret = toolTest.runTool.apply(toolTest, ['restore']
      .concat(writeConcern)
      .concat(getRestoreTarget(dumpTarget))
      .concat(commonToolArgs));
    assert.eq(exitCode, ret, name);
    dbOne.dropDatabase();
  }

  function noConnectTest() {
    return startMongoProgramNoConnect.apply(null, ['mongorestore',
        '--writeConcern={w:3}', '--host', st.s.host]
      .concat(getRestoreTarget(dumpTarget))
      .concat(commonToolArgs));
  }

  // drop the database so it's empty
  dbOne.dropDatabase();

  // load and run the write concern suite
  load('jstests/libs/wc_framework.js');
  runWCTest("mongorestore", rs, toolTest, writeConcernTestFunc, noConnectTest);

  dbOne.dropDatabase();
  rs.stopSet();
  toolTest.stop();

}());
