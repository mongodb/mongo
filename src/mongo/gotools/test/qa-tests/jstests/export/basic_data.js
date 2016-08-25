(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests running mongoexport with some basic data, and bringing it back
  // in with import.

  jsTest.log('Testing exporting, then importing, some basic data');

  var toolTest = getToolTest('basic_data');
  var commonToolArgs = getCommonToolArguments();

  // the export target
  var exportTarget = 'basic_data_export.json';
  removeFile(exportTarget);

  // the db and collection we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var testColl = testDB.data;

  // insert some data
  for (var i = 0; i < 50; i++) {
    testColl.insert({_id: i});
  }
  // sanity check the insertion worked
  assert.eq(50, testColl.count());

  // export the data
  var ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'data']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // drop the database
  testDB.dropDatabase();

  // import the data back in
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'data']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data is correct
  assert.eq(50, testColl.count());
  for (i = 0; i < 50; i++) {
    assert.eq(1, testColl.count({_id: i}));
  }

  // success
  toolTest.stop();

}());
