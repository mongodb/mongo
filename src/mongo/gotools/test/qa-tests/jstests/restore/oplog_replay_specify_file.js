/**
 * oplog_replay_specify_file.js
 *
 * This file tests mongorestore with --oplogReplay where the user specifies a file with the
 * --oplogFile flag.
 */
(function() {
  'use strict';
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var commonToolArgs = getCommonToolArguments();
  var dumpTarget = 'oplog_replay_specify_file';

  var toolTest = getToolTest('oplog_replay_specify_file');

  // The test db and collections we'll be using.
  var testDB = toolTest.db.getSiblingDB('test_oplog');
  var testColl = testDB.foo;
  var testRestoreDB = toolTest.db.getSiblingDB('test');
  var testRestoreColl = testRestoreDB.op;
  resetDbpath(dumpTarget);

  var oplogSize = 100;

  // Create a fake oplog consisting of 100 inserts.
  for (var i = 0; i < oplogSize; i++) {
    testColl.insert({
      ts: new Timestamp(0, i),
      op: "i",
      o: {_id: i, x: 'a' + i},
      ns: "test.op"
    });
  }

  // Dump the fake oplog.
  var ret = toolTest.runTool.apply(toolTest, ['dump',
      '--db', 'test_oplog',
      '-c', 'foo',
      '--out', dumpTarget]
    .concat(commonToolArgs));
  assert.eq(0, ret, "dump operation failed");

  // Dump original data.
  testColl.drop();
  assert.eq(0, testColl.count(),
      "all original entries should be dropped");

  // Create the test.op collection.
  testRestoreColl.drop();
  testRestoreDB.createCollection("op");
  assert.eq(0, testRestoreColl.count());

  // Replay the oplog from the provided oplog
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '--oplogReplay',
      '--oplogFile', dumpTarget + '/test_oplog/foo.bson',
      dumpTarget]
    .concat(commonToolArgs));
  assert.eq(0, ret, "restore operation failed");

  assert.eq(oplogSize, testRestoreColl.count(),
      "all oplog entries should be inserted");
  assert.eq(oplogSize, testColl.count(),
      "all original entries should be restored");
  toolTest.stop();
}());
