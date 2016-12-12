/**
 * oplog_replay_priority_oplog.js
 *
 * This file tests mongorestore with --oplogReplay where the user provides two oplogs and
 * mongorestore only restores the higher priority one.
 */
(function() {
  'use strict';
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var commonToolArgs = getCommonToolArguments();
  var restoreTarget = 'jstests/restore/testdata/dump_local_oplog';

  var toolTest = getToolTest('oplog_replay_priority_oplog');

  // The test db and collections we'll be using.
  var testDB = toolTest.db.getSiblingDB('test');
  testDB.createCollection('data');
  var testColl = testDB.data;
  testDB.createCollection('op');
  var restoreColl = testDB.op;

  // Replay the oplog from the provided oplog
  var ret = toolTest.runTool.apply(toolTest, ['restore',
      '--oplogReplay',
      '--oplogFile', 'jstests/restore/testdata/extra_oplog.bson',
      restoreTarget]
    .concat(commonToolArgs));
  assert.eq(0, ret, "restore operation failed");

  // Extra oplog has 5 entries as explained in oplog_replay_and_limit.js
  assert.eq(5, testColl.count(),
      "all original entries from high priority oplog should be restored");
  assert.eq(0, restoreColl.count(),
      "no original entries from low priority oplog should be restored");
  toolTest.stop();
}());

