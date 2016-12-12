/*
 * This test creates a fake oplog and uses it to test correct behavior of
 * all possible op codes.
 */
(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var OPLOG_INSERT_CODE = 'i';
  var OPLOG_COMMAND_CODE = 'c';
  var OPLOG_UPDATE_CODE = 'u';
  var OPLOG_REMOVE_CODE = 'd';
  var OPLOG_NOOP_CODE = 'n';
  var CURRENT_OPLOG_VERSION = 2;

  var toolTest = getToolTest('applyAllOpsTest');
  var commonToolArgs = getCommonToolArguments();

  // Get the db that we'll insert the fake oplog into
  var db = toolTest.db.getSiblingDB('foo');
  db.dropDatabase();
  db.getSiblingDB('rs').dropDatabase();

  // Create capped collection
  db.getSiblingDB('rs').createCollection('rs_test', {capped: true, size: 4});

  // Add a bunch of operations to the fake oplog

  // Create a collection to drop
  db.getSiblingDB('rs').rs_test.insert({
    ts: new Timestamp(),
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_COMMAND_CODE,
    ns: "foo.$cmd",
    o: {create: "baz"}
  });

  // Insert a doc
  db.getSiblingDB('rs').rs_test.insert({
    ts: new Timestamp(),
    h: 0,
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_INSERT_CODE,
    o: {
      _id: 0
    },
    ns: 'foo.baz'
  });

  // Drop the doc's database
  db.getSiblingDB('rs').rs_test.insert({
    ts: new Timestamp(),
    h: 1,
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_COMMAND_CODE,
    o: {
      dropDatabase: 1
    },
    ns: 'foo.$cmd'
  });

  // Create the collection
  db.getSiblingDB('rs').rs_test.insert({
    ts: new Timestamp(),
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_COMMAND_CODE,
    ns: "foo.$cmd",
    o: {create: "bar"}
  });

  // Insert 2 docs
  db.getSiblingDB('rs').rs_test.insert({
    ts: new Timestamp(),
    h: 2,
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_INSERT_CODE,
    o: {
      _id: 1
    },
    ns: 'foo.bar'
  });

  db.getSiblingDB('rs').rs_test.insert({
    ts: new Timestamp(),
    h: 3,
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_INSERT_CODE,
    o: {
      _id: 2
    },
    ns: 'foo.bar'
  });

  // Remove first doc
  db.getSiblingDB('rs').rs_test.insert({
    ts: new Timestamp(),
    h: 4,
    b: true,
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_REMOVE_CODE,
    o: {
      _id: 1
    },
    ns: 'foo.bar'
  });

  // Update the second doc
  db.getSiblingDB('rs').rs_test.insert({
    ts: new Timestamp(),
    h: 5,
    b: true,
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_UPDATE_CODE,
    o2: {
      _id: 2
    },
    o: {
      _id: 2,
      x: 1
    },
    ns: 'foo.bar'
  });

  // Noop
  db.getSiblingDB('rs').rs_test.insert({
    ts: new Timestamp(),
    h: 6,
    op: OPLOG_NOOP_CODE,
    ns: 'foo.bar',
    o: {x: 'noop'}
  });

  var args = ['oplog', '--oplogns', 'rs.rs_test',
    '--from', '127.0.0.1:' + toolTest.port].concat(commonToolArgs);

  if (toolTest.isSharded) {
    // When applying ops to a sharded cluster,
    assert(toolTest.runTool.apply(toolTest, args) !== 0,
      'mongooplog should fail when running applyOps on a sharded cluster');

    var expectedError =
      'error applying ops: applyOps not allowed through mongos';
    assert.strContains.soon(expectedError, rawMongoProgramOutput,
      'mongooplog crash should output the correct error message');

    assert.eq(0, db.bar.count({}),
      'mongooplog should not have applied any operations');
  } else {
    // Running with default --seconds should apply all operations
    assert.eq(toolTest.runTool.apply(toolTest, args), 0,
      'mongooplog should succeed');

    assert.eq(1, db.bar.count({}),
      'mongooplog should apply all operations');
    assert.eq(0, db.baz.count({}), 'mongooplog should have dropped db');
    assert.eq(1, db.bar.count({_id: 2}),
      'mongooplog should have applied correct ops');
  }

  toolTest.stop();
}());
