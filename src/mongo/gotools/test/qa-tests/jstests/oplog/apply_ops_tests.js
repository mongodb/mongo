/*
 * This test creates a fake oplog and uses it to test correct behavior of
 * --oplogns and --seconds
 */
(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var OPLOG_INSERT_CODE = 'i';
  var OPLOG_UPDATE_CODE = 'u';
  // unused: OPLOG_COMMAND_CODE = 'c';
  var CURRENT_OPLOG_VERSION = 2;

  // Oplog TS is in seconds since unix epoch
  var TEST_START = Math.floor(new Date().getTime() / 1000);
  var toolTest = getToolTest('oplogSuccessTest');
  var commonToolArgs = getCommonToolArguments();

  // Get the db that we'll insert the fake oplog into
  var db = toolTest.db.getSiblingDB('gnr');
  db.dropDatabase();

  // Create capped collection
  db.createCollection('rs_test', {capped: true, max: 4});
  // Create test collection
  db.createCollection('greatest_hits');

  // Add a bunch of operations to the fakeoplog
  var tracks = ['Welcome to the Jungle', 'Sweet Child O\' Mine', 'Patience',
    'Paradise City', 'Knockin\' on Heaven\'s Door', 'Civil War'];

  tracks.forEach(function(track, index) {
    db.rs_test.insert({
      ts: new Timestamp(TEST_START - index * 10000 - 1, 1),
      h: index,
      v: CURRENT_OPLOG_VERSION,
      op: OPLOG_INSERT_CODE,
      o: {
        _id: track
      },
      ns: 'gnr.greatest_hits'
    });
  });

  tracks.forEach(function(track, index) {
    db.rs_test.insert({
      ts: new Timestamp(TEST_START - index * 10000 - 1, 2),
      h: index,
      v: CURRENT_OPLOG_VERSION,
      op: OPLOG_UPDATE_CODE,
      o2: {
        _id: track
      },
      o: {
        _id: track,
        index: index
      },
      ns: 'gnr.greatest_hits'
    });
  });

  var args = ['oplog', '--oplogns', 'gnr.rs_test',
    '--from', '127.0.0.1:' + toolTest.port].concat(commonToolArgs);

  assert.eq(0, db.getSiblingDB('gnr').greatest_hits.count({}),
    'target collection should be empty before mongooplog runs');

  if (toolTest.isSharded) {
    // When applying ops to a sharded cluster,
    assert(toolTest.runTool.apply(toolTest, args) !== 0,
      'mongooplog should fail when running applyOps on a sharded cluster');

    var expectedError =
      'error applying ops: applyOps not allowed through mongos';
    assert.strContains.soon(expectedError, rawMongoProgramOutput,
      'mongooplog crash should output the correct error message');

    assert.eq(0, db.greatest_hits.count({}),
      'mongooplog should not have applied any operations');
  } else {
    // Running with default --seconds should apply all operations
    assert.eq(toolTest.runTool.apply(toolTest, args), 0,
      'mongooplog should succeed');

    assert.eq(6, db.greatest_hits.count({}),
      'mongooplog should apply all operations');
    tracks.forEach(function(track, index) {
      assert.eq(1, db.greatest_hits.count({_id: track, index: index}),
        'mongooplog should have inserted a doc with _id="' + track + '" and ' +
        'updated it to have index=' + index);
    });

    // Running a second time should have no effect
    assert.eq(toolTest.runTool.apply(toolTest, args), 0,
      'mongooplog should succeed');
    assert.eq(6, db.greatest_hits.count({}),
      'mongooplog should apply all operations');
    tracks.forEach(function(track, index) {
      assert.eq(1, db.greatest_hits.count({_id: track, index: index}),
        'mongooplog should have inserted a doc with _id="' + track + '" and ' +
        'updated it to have index=' + index);
    });

    db.greatest_hits.drop();
    db.createCollection('greatest_hits');

    // Running with `--seconds 25000` should apply last 3 operations, which
    // have timestamps T - 1, T - 10001, and T - 20001 (roughly)
    var last3Seconds = args.concat(['--seconds', 25000]);
    assert.eq(toolTest.runTool.apply(toolTest, last3Seconds), 0,
      'mongooplog should succeed');

    assert.eq(3, db.greatest_hits.count({}),
      '`mongooplog --seconds 25000` should apply 3 operations');
    tracks.slice(0, 3).forEach(function(track, index) {
      assert.eq(1, db.greatest_hits.count({_id: track, index: index}),
        'mongooplog should have inserted a doc with _id="' + track + '" and ' +
        'updated it to have index=' + index);
    });

    db.greatest_hits.drop();
    db.createCollection('greatest_hits');

    // Running with `--seconds 0` should apply no operations
    var noOpsArgs = args.concat(['--seconds', 0]);
    assert.eq(toolTest.runTool.apply(toolTest, noOpsArgs), 0,
      'mongooplog should succeed');

    assert.eq(0, db.greatest_hits.count({}),
      '`mongooplog --seconds 0` should apply 0 operations');
  }

  toolTest.stop();
}());
