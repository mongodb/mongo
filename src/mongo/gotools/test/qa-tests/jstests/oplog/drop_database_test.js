/*
 * Tests behavior when oplog contains an operation to drop itself
 */
(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var OPLOG_INSERT_CODE = 'i';
  var OPLOG_COMMAND_CODE = 'c';
  var CURRENT_OPLOG_VERSION = 2;

  var toolTest = getToolTest('oplogDropDbTest');
  var commonToolArgs = getCommonToolArguments();

  // Get the db that we'll insert the fake oplog into
  var db = toolTest.db.getSiblingDB('foo');
  db.dropDatabase();

  // Create capped collection on foo
  db.createCollection('rs_test', {capped: true, size: 4});

  // Create test collection
  db.createCollection("baz");

  // Insert a doc
  db.rs_test.insert({
    ts: new Timestamp(),
    h: 0,
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_INSERT_CODE,
    o: {
      _id: 0
    },
    ns: 'foo.baz'
  });

  // Drop foo, which also includes the rs_test collection that the oplog is in
  db.rs_test.insert({
    ts: new Timestamp(),
    h: 1,
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_COMMAND_CODE,
    o: {
      dropDatabase: 1
    },
    ns: 'foo.$cmd'
  });

  // Recreate collection
  db.rs_test.insert({
    ts: new Timestamp(),
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_COMMAND_CODE,
    ns: "foo.$cmd",
    o: {create: "baz"},
  });

  // Insert another doc
  db.rs_test.insert({
    ts: new Timestamp(),
    h: 2,
    v: CURRENT_OPLOG_VERSION,
    op: OPLOG_INSERT_CODE,
    o: {
      _id: 1
    },
    ns: 'foo.baz'
  });

  var args = ['oplog', '--oplogns', 'foo.rs_test',
    '--from', '127.0.0.1:' + toolTest.port].concat(commonToolArgs);

  if (toolTest.isSharded) {
    // When applying ops to a sharded cluster,
    assert(toolTest.runTool.apply(toolTest, args) !== 0,
      'mongooplog should fail when running applyOps on a sharded cluster');

    var expectedError =
      'error applying ops: applyOps not allowed through mongos';
    assert.strContains.soon(expectedError, rawMongoProgramOutput,
      'mongooplog crash should output the correct error message');

    assert.eq(0, db.baz.count({}),
      'mongooplog should not have applied any operations');
  } else {
    // Running with default --seconds should apply all operations
    assert.eq(toolTest.runTool.apply(toolTest, args), 0,
      'mongooplog should succeed');

    assert.eq(1, db.baz.count({_id: 1}), 'should have restored the document');
  }

  toolTest.stop();
}());
