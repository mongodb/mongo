/**
 * oplog_replay_local_main.js
 *
 * This file tests mongorestore with --oplogReplay where the oplog file is in the 'oplog.$main'
 * collection of the 'local' database. This occurs when using master-slave replication.
 */
(function() {
  'use strict';

  var dumpTarget = 'oplog_replay_local_main';
  var rt = new ReplTest('oplog_replay_local_main');
  var m = rt.start(true);
  // Set the test db to 'local' and collection to 'oplog.$main' to fake a replica set oplog
  var testDB = m.getDB('local');
  var testColl = testDB.oplog.$main;
  var testRestoreDB = m.getDB('test');
  var testRestoreColl = testRestoreDB.op;
  resetDbpath(dumpTarget);

  var lastop = function() {
    return testColl.find().sort({$natural: -1}).next();
  };

  var lastTS = lastop().ts.t;
  var oplogSize = 100;

  // Create a fake oplog consisting of 100 inserts.
  for (var i = 0; i < oplogSize; i++) {
    var op = {
      ts: new Timestamp(lastTS, i),
      op: 'i',
      o: {_id: i, x: 'a' + i},
      ns: 'test.op'
    };
    assert.commandWorked(testDB.runCommand({godinsert: 'oplog.$main', obj: op}));
  }

  // Dump the fake oplog.
  var ret = runMongoProgram('mongodump',
      '--port', rt.ports[0],
      '--db', 'local',
      '-c', 'oplog.$main',
      '--out', dumpTarget);
  assert.eq(0, ret, "dump operation failed");

  // Create the test.op collection.
  testRestoreColl.drop();
  testRestoreDB.createCollection("op");
  assert.eq(0, testRestoreColl.count());

  // Replay the oplog from the provided oplog
  ret = runMongoProgram('mongorestore',
      '--port', rt.ports[0],
      '--oplogReplay',
      dumpTarget);
  assert.eq(0, ret, "restore operation failed");

  assert.eq(oplogSize, testRestoreColl.count(), "all oplog entries should be inserted");
  rt.stop(true);
}());
