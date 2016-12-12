/*
 * Tests correct behavior when operating against a live oplog
 */
(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var toolTest = getToolTest('oplogRealOplogTest');
  var commonToolArgs = getCommonToolArguments();

  // Get the db that we'll insert operations into
  var db = toolTest.db.getSiblingDB('gnr');
  db.dropDatabase();

  // Sleep for a long time so we can safely use --seconds to get the
  // right operations to verify that the `dropDatabase` and subsequent
  // inserts and updates get applied
  db.test.insert({x: 1});

  var LONG_SLEEP_TIME = 5000;
  sleep(LONG_SLEEP_TIME);

  db.dropDatabase();

  // Do 6 inserts and 6 updates
  var tracks = ['Welcome to the Jungle', 'Sweet Child O\' Mine', 'Patience',
    'Paradise City', 'Knockin\' on Heaven\'s Door', 'Civil War'];

  tracks.forEach(function(track) {
    db.greatest_hits.insert({
      _id: track
    });
  });

  tracks.forEach(function(track, index) {
    db.greatest_hits.update({_id: track}, {$set: {index: index}});
  });

  var args = ['oplog', '--seconds', '1',
    '--from', '127.0.0.1:' + toolTest.port].concat(commonToolArgs);

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
    // Running should apply the drop followed by 6 updates and 6 inserts,
    // but not the { x: 1 } insert.
    assert.eq(toolTest.runTool.apply(toolTest, args), 0,
      'mongooplog should succeed');

    assert.eq(6, db.greatest_hits.count({}),
      'mongooplog should apply all operations');
    assert.eq(0, db.test.count(), 'mongooplog should not have restored an ' +
      'insert that happened before the --seconds cutoff');
    tracks.forEach(function(track, index) {
      assert.eq(1, db.greatest_hits.count({_id: track, index: index}),
        'mongooplog should have inserted a doc with _id="' + track + '" and ' +
        'updated it to have index=' + index);
    });
  }

  toolTest.stop();
}());
