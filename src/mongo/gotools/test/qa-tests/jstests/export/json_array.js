(function() {

  // Tests running mongoexport with the --jsonArray output option.

  jsTest.log('Testing exporting with --jsonArray specified');

  var toolTest = new ToolTest('json_array');
  toolTest.startDB('foo');

  // the db and collection we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var testColl = testDB.data;

  // the export target
  var exportTarget = 'json_array_export.json';
  removeFile(exportTarget);

  // insert some data
  for (var i = 0; i < 20; i++) {
    testColl.insert({_id: i});
  }
  // sanity check the insertion worked
  assert.eq(20, testColl.count());

  // export the data
  var ret = toolTest.runTool('export', '--out', exportTarget,
      '--db', 'test', '--collection', 'data', '--jsonArray');
  assert.eq(0, ret);

  // drop the data
  testDB.dropDatabase();

  // make sure that mongoimport without --jsonArray does not work
  ret = toolTest.runTool('import', '--file', exportTarget,
      '--db', 'test', '--collection', 'data');
  assert.neq(0, ret);

  // make sure nothing was imported
  assert.eq(0, testColl.count());

  // run mongoimport again, with --jsonArray
  ret = toolTest.runTool('import', '--file', exportTarget,
      '--db', 'test', '--collection', 'data', '--jsonArray');
  assert.eq(0, ret);

  // make sure the data was imported
  assert.eq(20, testColl.count());
  for (i = 0; i < 20; i++) {
    assert.eq(1, testColl.count({_id: i}));
  }

  // success
  toolTest.stop();

}());
