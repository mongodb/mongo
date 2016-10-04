/**
 * no_primary_error_code.js
 *
 * This file tests TOOLS-690 where mongoimport returned exit code 0 when it should have returned
 * exit code 1 on error. The error stems from when mongos cannot find a primary. This file checks
 * that errors of type 'not master', 'unable to target', and 'Connection refused' yield error
 * code 1.
 */
(function() {
  'use strict';
  jsTest.log('Testing mongoimport when a sharded cluster has no primaries');

  var sh = new ShardingTest({
    name: 'no_primary_error_code',
    shards: 1,
    verbose: 0,
    mongos: 1,
    other: {
      rs: true,
      numReplicas: 1,
      chunksize: 1,
      enableBalancer: 0,
    },
  });

  // If we can't find a primary in 20 seconds than assume there are no more.
  var primary = sh.rs0.getPrimary(20000);

  jsTest.log('Stepping down ' + primary.host);

  try {
    primary.adminCommand({replSetStepDown: 300, force: true});
  } catch (e) {
    // Ignore any errors that occur when stepping down the primary.
    print('Error Stepping Down Primary: ' + e);
  }

  // Check that we catch 'not master'
  jsTest.log('All primaries stepped down, trying to import.');


  var ret = runMongoProgram('mongoimport',
      '--file', 'jstests/import/testdata/basic.json',
      '--db', 'test',
      '--collection', 'noPrimaryErrorCode',
      '--host', sh.s0.host);
  assert.eq(ret, 1, 'mongoimport should fail with no primary');

  sh.getDB('test').dropDatabase();

  // Kill the replica set.
  sh.rs0.stopSet(15);

  // Check that we catch 'Connection refused'
  jsTest.log('All primaries died, trying to import.');

  ret = runMongoProgram('mongoimport',
      '--file', 'jstests/import/testdata/basic.json',
      '--db', 'test',
      '--collection', 'noPrimaryErrorCode',
      '--host', sh.s0.host);
  assert.eq(ret, 1, 'mongoimport should fail with no primary');

  sh.stop();
}());
