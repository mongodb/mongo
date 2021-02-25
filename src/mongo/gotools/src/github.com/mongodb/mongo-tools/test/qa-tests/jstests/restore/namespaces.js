(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  jsTest.log('Testing namespaces escludes, includes, and mappings during restore');

  var toolTest = getToolTest('namespaces');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'namespaces_dump';

  // the db we will dump from
  var source1DB = toolTest.db.getSiblingDB('source1');
  var source2DB = toolTest.db.getSiblingDB('source2');
  var source3DB = toolTest.db.getSiblingDB('source3');
  // the db we will restore to
  var destDB = toolTest.db.getSiblingDB('dest');

  function performRestoreWithArgs(...args) {
    return toolTest.runTool.apply(toolTest, ['restore']
      .concat(args)
      .concat(getRestoreTarget(dumpTarget))
      .concat(commonToolArgs));
  }

  function addTestDataTo(db, colls) {
    colls.forEach(function(coll) {
      var data = [];
      for (var i = 0; i < 500; i++) {
        data.push({_id: i+'_'+db.getName()+'.'+coll});
      }
      db[coll].insertMany(data);
      // sanity check the insertion worked
      assert.eq(500, db[coll].count());
      // Add an index
      var index = {};
      index[db.getName()+'.'+coll] = 1;
      db[coll].createIndex(index);
    });
  }

  function verifyDataIn(collection, sourceNS) {
    if (sourceNS === null) {
      assert.eq(0, collection.count());
      return;
    }
    assert.eq(500, collection.count());
    for (var i = 0; i < 500; i++) {
      assert.eq(1, collection.count({_id: i+'_'+sourceNS}));
    }
    assert.eq(1, collection.getIndexes()[1].key[sourceNS]);
  }

  addTestDataTo(source1DB, ['coll1', 'coll2', 'coll3']);
  verifyDataIn(source1DB.coll1, 'source1.coll1');
  verifyDataIn(source1DB.coll2, 'source1.coll2');
  verifyDataIn(source1DB.coll3, 'source1.coll3');

  addTestDataTo(source2DB, ['coll1', 'coll2', 'coll3']);
  verifyDataIn(source2DB.coll1, 'source2.coll1');
  verifyDataIn(source2DB.coll2, 'source2.coll2');
  verifyDataIn(source2DB.coll3, 'source2.coll3');

  addTestDataTo(source3DB, ['coll3', 'coll4']);
  verifyDataIn(source3DB.coll3, 'source3.coll3');
  verifyDataIn(source3DB.coll4, 'source3.coll4');

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // Get rid of the source databases
  source1DB.dropDatabase();
  source2DB.dropDatabase();
  source3DB.dropDatabase();

  // Exclude *.coll1
  ret = performRestoreWithArgs('--nsExclude', '*.coll1', '--nsFrom', 'source$db-num$.coll$coll-num$', '--nsTo', 'dest.coll_$db-num$_$coll-num$');
  assert.eq(0, ret);

  verifyDataIn(destDB.coll_1_1, null);
  verifyDataIn(destDB.coll_1_2, 'source1.coll2');
  verifyDataIn(destDB.coll_1_3, 'source1.coll3');
  verifyDataIn(destDB.coll_2_1, null);
  verifyDataIn(destDB.coll_2_2, 'source2.coll2');
  verifyDataIn(destDB.coll_2_3, 'source2.coll3');
  verifyDataIn(destDB.coll_3_1, null);
  verifyDataIn(destDB.coll_3_2, null);
  verifyDataIn(destDB.coll_3_3, 'source3.coll3');
  verifyDataIn(destDB.coll_3_4, 'source3.coll4');

  destDB.dropDatabase();

  // Inclode only *.coll1
  ret = performRestoreWithArgs('--nsInclude', '*.coll1', '--nsFrom', 'source$db-num$.coll$coll-num$', '--nsTo', 'dest.coll_$db-num$_$coll-num$');
  assert.eq(0, ret);

  verifyDataIn(destDB.coll_1_1, 'source1.coll1');
  verifyDataIn(destDB.coll_1_2, null);
  verifyDataIn(destDB.coll_1_3, null);
  verifyDataIn(destDB.coll_2_1, 'source2.coll1');
  verifyDataIn(destDB.coll_2_2, null);
  verifyDataIn(destDB.coll_2_3, null);
  verifyDataIn(destDB.coll_3_1, null);
  verifyDataIn(destDB.coll_3_2, null);
  verifyDataIn(destDB.coll_3_3, null);
  verifyDataIn(destDB.coll_3_4, null);

  destDB.dropDatabase();

  // Exclude collections beginning with 'coll' (which is all of them)
  ret = performRestoreWithArgs('--excludeCollectionsWithPrefix', 'coll', '--nsFrom', 'source$db-num$.coll$coll-num$', '--nsTo', 'dest.coll_$db-num$_$coll-num$');
  assert.eq(0, ret);

  verifyDataIn(destDB.coll_1_1, null);
  verifyDataIn(destDB.coll_1_2, null);
  verifyDataIn(destDB.coll_1_3, null);
  verifyDataIn(destDB.coll_2_1, null);
  verifyDataIn(destDB.coll_2_2, null);
  verifyDataIn(destDB.coll_2_3, null);
  verifyDataIn(destDB.coll_3_1, null);
  verifyDataIn(destDB.coll_3_2, null);
  verifyDataIn(destDB.coll_3_3, null);
  verifyDataIn(destDB.coll_3_4, null);

  destDB.dropDatabase();

  // Swap source1 and source2 databases
  ret = performRestoreWithArgs('--nsFrom', 'source1.*', '--nsTo', 'source2.*', '--nsFrom', 'source2.*', '--nsTo', 'source1.*');
  assert.eq(0, ret);

  verifyDataIn(source1DB.coll1, 'source2.coll1');
  verifyDataIn(source1DB.coll2, 'source2.coll2');
  verifyDataIn(source1DB.coll3, 'source2.coll3');
  verifyDataIn(source2DB.coll1, 'source1.coll1');
  verifyDataIn(source2DB.coll2, 'source1.coll2');
  verifyDataIn(source2DB.coll3, 'source1.coll3');
  verifyDataIn(source3DB.coll3, 'source3.coll3');
  verifyDataIn(source3DB.coll4, 'source3.coll4');

  source1DB.dropDatabase();
  source2DB.dropDatabase();
  source3DB.dropDatabase();

  toolTest.stop();

}());
