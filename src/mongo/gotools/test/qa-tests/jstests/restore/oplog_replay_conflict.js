/**
 * oplog_replay_conflict.js
 *
 * This file tests mongorestore with --oplogReplay where the user provides two top priority
 * oplogs and mongorestore should exit with an error.
 */
(function() {
  'use strict';
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var commonToolArgs = getCommonToolArguments();
  var restoreTarget = 'jstests/restore/testdata/dump_oplog_conflict';

  var toolTest = getToolTest('oplog_replay_conflict');

  // The test db and collections we'll be using.
  var testDB = toolTest.db.getSiblingDB('test');
  testDB.createCollection('data');
  var testColl = testDB.data;

  // Replay the oplog from the provided oplog
  var ret = toolTest.runTool.apply(toolTest, ['restore',
      '--oplogReplay',
      '--oplogFile', 'jstests/restore/testdata/extra_oplog.bson',
      restoreTarget].concat(commonToolArgs));

  assert.eq(0, testColl.count(),
            "no original entries should be restored");
  assert.eq(1, ret, "restore operation succeeded when it shouldn't have");
  toolTest.stop();
}());
