(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests that mongorestore correctly restores collections with a default collation.

  jsTest.log('Testing restoration of a collection with a default collation');

  var toolTest = getToolTest('collation');
  var commonToolArgs = getCommonToolArguments();

  var dumpTarget = 'collation_dump';
  resetDbpath(dumpTarget);

  var testDB = toolTest.db.getSiblingDB('test');
  var testColl = testDB.coll;

  // Create a collection with a default collation.
  assert.commandWorked(testDB.createCollection('coll', {collation: {locale: 'fr_CA'}}));
  var collectionInfos = testDB.getCollectionInfos({name: 'coll'});
  assert.eq(collectionInfos.length, 1);
  assert(collectionInfos[0].options.hasOwnProperty('collation'), tojson(collectionInfos[0]));
  var collationBefore = collectionInfos[0].options.collation;

  // Dump the data.
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // Drop the collection.
  testColl.drop();

  // Restore the data.
  ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  collectionInfos = testDB.getCollectionInfos({name: 'coll'});
  assert.eq(collectionInfos.length, 1);
  assert(collectionInfos[0].options.hasOwnProperty('collation'), tojson(collectionInfos[0]));
  var collationAfter = collectionInfos[0].options.collation;

  // Check that the collection was restored with the same collation.
  assert.docEq(collationBefore, collationAfter, tojson(collationBefore) + tojson(collationAfter));

  if (dump_targets === 'archive') {
    jsTest.log('skipping bson file restore test while running with archiving');
  } else {
    // Drop the collection.
    testColl.drop();

    // Restore the data, but this time mentioning the bson file specifically.
    ret = toolTest.runTool.apply(toolTest, ['restore']
      .concat(getRestoreTarget(dumpTarget+'/test/coll.bson'))
      .concat(commonToolArgs));
    assert.eq(0, ret);

    collectionInfos = testDB.getCollectionInfos({name: 'coll'});
    assert.eq(collectionInfos.length, 1);
    assert(collectionInfos[0].options.hasOwnProperty('collation'), tojson(collectionInfos[0]));
    collationAfter = collectionInfos[0].options.collation;

    // Check that the collection was restored with the same collation.
    assert.docEq(collationBefore, collationAfter, tojson(collationBefore) + tojson(collationAfter));
  }

  // success
  toolTest.stop();
}());
