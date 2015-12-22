(function() {

  load("jstests/configs/replset_28.config.js");

  var name = 'import_write_concern';
  var toolTest = new ToolTest(name, null);
  var dbName = "foo";
  var colName = "bar";
  rs = new ReplSetTest({
    name: name,
    nodes: 3,
    useHostName: true
  });

  var commonToolArgs = getCommonToolArguments();
  var fileTarget = "wc.csv"
  rs.startSet({});
  var cfg = rs.getReplSetConfig();
  cfg.settings = {};
  cfg.settings.chainingAllowed = false;
  rs.initiate(cfg);
  rs.awaitReplication();
  toolTest.port = rs.getPrimary().port;

  function writeConcernTestFunc(exitCode,writeConcern,name) {
    jsTest.log(name);
    ret = toolTest.runTool.apply(
        toolTest,
        ['import','--file',fileTarget, '-d', dbName, '-c', colName].
        concat(writeConcern).
        concat(commonToolArgs)
        );
    assert.eq(exitCode, ret, name);
    db.dropDatabase();
  }

  function noConnectTest() {
    return startMongoProgramNoConnect.apply(null,
        ['mongoimport','--writeConcern={w:3}','--host',rs.getPrimary().host,'--file',fileTarget].
        concat(commonToolArgs)
        );
  }

  // create a test collection
  var db = rs.getPrimary().getDB(dbName);
  var col = db.getCollection(colName);
  for(var i=0;i<=100;i++){
    col.insert({_id:i, x:i*i});
  }
  rs.awaitReplication();

  // export the data that we'll use
  var ret = toolTest.runTool.apply(
      toolTest,
      ['export','--out',fileTarget, '-d', dbName, '-c', colName].
      concat(commonToolArgs)
      );
  assert.eq(0, ret);

  // drop the database so it's empty
  db.dropDatabase();

  // load and run the write concern suite
  load('jstests/libs/wc_framework.js');
  runWCTest("mongoimport", rs, toolTest, writeConcernTestFunc, noConnectTest);

  db.dropDatabase();
  rs.stopSet();
  toolTest.stop();

}());
