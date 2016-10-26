(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests using mongorestore to restore data to a different db than
  // it was dumped from.

  jsTest.log('Testing restoration to a different db');

  if (dump_targets === 'archive') {
    jsTest.log('Skipping test unsupported against archive targets');
    return assert(true);
  }

  var toolTest = getToolTest('different_db');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'different_db_dump';
  resetDbpath(dumpTarget);

  // the db we will dump from
  var sourceDB = toolTest.db.getSiblingDB('source');
  // the db we will restore to
  var destDB = toolTest.db.getSiblingDB('dest');

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // we'll use two collections
  var collNames = ['coll1', 'coll2'];

  // insert a bunch of data
  collNames.forEach(function(collName) {
    var data = [];
    for (var i = 0; i < 500; i++) {
      data.push({_id: i+'_'+collName});
    }
    sourceDB[collName].insertMany(data);
    // sanity check the insertion worked
    assert.eq(500, sourceDB[collName].count());
  });

  // dump the data
  ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // restore the data to a different db
  ret = toolTest.runTool.apply(toolTest, ['restore', '--db', 'dest']
    .concat(getRestoreTarget(dumpTarget+'/source'))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored
  collNames.forEach(function(collName) {
    assert.eq(500, destDB[collName].count());
    for (var i = 0; i < 500; i++) {
      assert.eq(1, destDB[collName].count({_id: i+'_'+collName}));
    }
  });

  // restore the data to another different db
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '--nsFrom', '$db$.$collection$',
      '--nsTo', 'otherdest.$db$_$collection$']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);
  destDB = toolTest.db.getSiblingDB('otherdest');
  collNames.forEach(function(collName) {
    assert.eq(500, destDB['source_'+collName].count());
    for (var i = 0; i < 500; i++) {
      assert.eq(1, destDB['source_'+collName].count({_id: i+'_'+collName}));
    }
  });

  // success
  toolTest.stop();

}());
