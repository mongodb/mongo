(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var toolTest = new ToolTest('write_concern', null);
  var commonToolArgs = getCommonToolArguments();

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
  var dbOne = st.s.getDB('dbOne');
  function writeConcernTestFunc(exitCode, writeConcern, name) {
    jsTest.log(name);
    ret = toolTest.runTool.apply(toolTest, ['files',
        '-vvvvv',
        '-d', 'dbOne']
      .concat(writeConcern)
      .concat(commonToolArgs)
      .concat(['put', 'jstests/files/testdata/files1.txt']));
    assert.eq(exitCode, ret, name);
    dbOne.dropDatabase();
  }

  function noConnectTest() {
    return startMongoProgramNoConnect.apply(null, ['mongofiles',
        '-d', 'dbOne',
        '--writeConcern={w:3}',
        '--host', st.s.host]
      .concat(commonToolArgs)
      .concat(['put', 'jstests/files/testdata/files1.txt']));
  }

  // drop the database so it's empty
  dbOne.dropDatabase();

  // load and run the write concern suite
  load('jstests/libs/wc_framework.js');
  runWCTest("mongofiles", rs, toolTest, writeConcernTestFunc, noConnectTest);

  dbOne.dropDatabase();
  rs.stopSet();
  toolTest.stop();

}());
