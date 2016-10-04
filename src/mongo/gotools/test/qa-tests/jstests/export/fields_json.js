(function() {

  if (typeof getToolTest === "undefined") {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests running mongoexport exporting to json with the --fields option

  jsTest.log('Testing exporting to json using the --fields option');

  var toolTest = getToolTest('fields_json');
  var commonToolArgs = getCommonToolArguments();

  // the db and collections we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var sourceColl = testDB.source;
  var destColl = testDB.dest;

  // the export target
  var exportTarget = 'fields_export.json';
  removeFile(exportTarget);

  // insert some data
  sourceColl.insert({a: 1});
  sourceColl.insert({a: 1, b: 1});
  sourceColl.insert({a: 1, b: 2, c: 3});
  // sanity check the insertion worked
  assert.eq(3, sourceColl.count());

  // export the data, specifying only one field
  var ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--fields', 'a']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // import the data into the destination collection
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest',
      '--type', 'json']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure only the specified field was exported
  assert.eq(3, destColl.count({a: 1}));
  assert.eq(0, destColl.count({b: 1}));
  assert.eq(0, destColl.count({b: 2}));
  assert.eq(0, destColl.count({c: 3}));

  // remove the export, clear the destination collection
  removeFile(exportTarget);
  destColl.remove({});

  // export the data, specifying all fields
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--fields', 'a,b,c']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // import the data into the destination collection
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest',
      '--type', 'json']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure everything was exported
  assert.eq(3, destColl.count({a: 1}));
  assert.eq(1, destColl.count({b: 1}));
  assert.eq(1, destColl.count({b: 2}));
  assert.eq(1, destColl.count({c: 3}));

  // make sure the _id was exported - the _id for the
  // corresponding documents in the two collections should
  // be the same
  var fromSource = sourceColl.findOne({a: 1, b: 1});
  var fromDest = destColl.findOne({a: 1, b: 1});
  assert.eq(fromSource._id, fromDest._id);

  // success
  toolTest.stop();

}());
