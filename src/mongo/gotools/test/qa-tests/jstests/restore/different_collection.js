(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests using mongorestore to restore data to a different collection
  // then it was dumped from.

  jsTest.log('Testing restoration to a different collection');

  if (dump_targets === 'archive') {
    jsTest.log('Skipping test unsupported against archive targets');
    return assert(true);
  }

  var toolTest = getToolTest('different_collection');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'different_collection_dump';
  resetDbpath(dumpTarget);

  // the db we will dump from
  var sourceDB = toolTest.db.getSiblingDB('source');
  // the collection we will dump from
  var sourceCollName = 'sourceColl';

  // insert a bunch of data
  for (var i = 0; i < 500; i++) {
    sourceDB[sourceCollName].insert({_id: i});
  }
  // sanity check the insertion worked
  assert.eq(500, sourceDB[sourceCollName].count());

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump'].concat(getDumpTarget(dumpTarget)));
  assert.eq(0, ret);

  // restore just the collection into a different collection
  // in the same database
  var destCollName = 'destColl';
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '--db', 'source',
      '--collection', destCollName]
    .concat(getRestoreTarget(dumpTarget+'/source/sourceColl.bson'))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored correctly
  assert.eq(500, sourceDB[destCollName].count());
  for (i = 0; i < 500; i++) {
    assert.eq(1, sourceDB[destCollName].count({_id: i}));
  }

  // restore just the collection into a similarly-named collection
  // in a different database
  var destDB = toolTest.db.getSiblingDB('dest');
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '--db', 'dest',
      '--collection', sourceCollName]
    .concat(getRestoreTarget(dumpTarget+'/source/sourceColl.bson'))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored correctly
  assert.eq(500, destDB[sourceCollName].count());
  for (i = 0; i < 500; i++) {
    assert.eq(1, destDB[sourceCollName].count({_id: i}));
  }

  // restore just the collection into a different collection
  // in a different database
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '--db', 'dest',
      '--collection', destCollName]
    .concat(getRestoreTarget(dumpTarget+'/source/sourceColl.bson'))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored correctly
  assert.eq(500, destDB[destCollName].count());
  for (i = 0; i < 500; i++) {
    assert.eq(1, destDB[destCollName].count({_id: i}));
  }

  // success
  toolTest.stop();

}());
