(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var commonToolArgs = getCommonToolArguments();
  var dumpTarget = 'oplog_replay_sizes';

  // Helper for using mongorestore with --oplogReplay and a large oplog.bson
  function tryOplogReplay(oplogSize, documentSize) {
    var toolTest = getToolTest('oplog_replay_sizes');
    // the test db and collections we'll be using
    var testDB = toolTest.db.getSiblingDB('test_oplog');
    var testColl = testDB.oplog;
    var testRestoreDB = toolTest.db.getSiblingDB('test');
    var testRestoreColl = testRestoreDB.op;
    resetDbpath(dumpTarget);

    var debugString = 'with ' + oplogSize + ' ops of size ' + documentSize;
    jsTest.log('Testing --oplogReplay ' + debugString);


    // create a fake oplog consisting of a large number of inserts
    var xStr = new Array(documentSize).join("x"); // ~documentSize bytes string
    var data = [];
    for (var i = 0; i < oplogSize; i++) {
      data.push({
        ts: new Timestamp(0, i),
        op: "i",
        o: {_id: i, x: xStr},
        ns: "test.op"
      });
      if (data.length === 1000) {
        testColl.insertMany(data);
        data = [];
      }
    }
    testColl.insertMany(data);

    // dump the fake oplog
    var ret = toolTest.runTool.apply(toolTest, ['dump',
        '--db', 'test_oplog',
        '-c', 'oplog',
        '--out', dumpTarget]
      .concat(commonToolArgs));
    assert.eq(0, ret, "dump operation failed " + debugString);

    // create the test.op collection
    testRestoreColl.drop();
    testRestoreDB.createCollection("op");
    assert.eq(0, testRestoreColl.count());

    // trick restore into replaying the "oplog" we forged above
    ret = toolTest.runTool.apply(toolTest, ['restore',
        '--oplogReplay', dumpTarget+'/test_oplog']
      .concat(commonToolArgs));
    assert.eq(0, ret, "restore operation failed " + debugString);
    assert.eq(oplogSize, testRestoreColl.count(),
          "all oplog entries should be inserted " + debugString);
    toolTest.stop();
  }

  // run the test on various oplog and op sizes
  tryOplogReplay(1024, 1024);      // sanity check
  tryOplogReplay(1024*1024, 1);    // millions of micro ops
  tryOplogReplay(8, 16*1024*1023); // 8 ~16MB ops
  tryOplogReplay(32, 1024*1024);   // 32 ~1MB ops
  tryOplogReplay(32*1024, 1024);   // many ~1KB ops

}());
