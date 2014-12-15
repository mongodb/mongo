if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

var OPLOG_INSERT_CODE = 'i';
var OPLOG_UPDATE_CODE = 'u';
var CURRENT_OPLOG_VERSION = 2;

// Oplog TS is in seconds since unix epoch
var TEST_START = Math.floor(new Date().getTime() / 1000);

(function() {
  var toolTest = getToolTest('oplogSuccessTest');
  var commonToolArgs = getCommonToolArguments();

  // Get local db so we can insert into a fake oplog
  var db = toolTest.db.getSiblingDB('gnr');
  db.dropDatabase();

  // Create capped collection
  db.createCollection('rs_test', { capped: true, size: 4 });

  // Add a bunch of operations to the fakeoplog
  var tracks = ['Welcome to the Jungle', 'Sweet Child O\' Mine', 'Patience',
    'Paradise City', 'Knockin\' on Heaven\'s Door', 'Civil War'];

  tracks.forEach(function(track, index) {
    db.rs_test.insert({
      ts: new Timestamp(TEST_START + index, 1),
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
      ts: new Timestamp(TEST_START + index, 1),
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

    var output = rawMongoProgramOutput();
    var expectedError =
      'error applying ops: applyOps not allowed through mongos';
    assert(output.indexOf(expectedError) !== -1,
      'mongodump crash should output the correct error message');

    assert.eq(0, db.greatest_hits.count({}),
      'mongooplog should not have applied any operations');
  } else {
    assert.eq(toolTest.runTool.apply(toolTest, args), 0,
      'mongooplog should succeed');

    assert.eq(6, db.greatest_hits.count({}),
      'mongooplog should apply all operations');
    tracks.forEach(function(track, index) {
      assert.eq(1, db.greatest_hits.count({ _id: track, index: index }),
        'mongooplog should have inserted a doc with _id="' + track + '" and ' +
        'updated it to have index=' + index);
    });
  }

  toolTest.stop();
})();
