(function() {

  if (typeof getToolTest === "undefined") {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests running mongoexport with --limit specified.

  jsTest.log('Testing exporting with --limit');

  var toolTest = getToolTest('limit');
  var commonToolArgs = getCommonToolArguments();

  // the export target
  var exportTarget = 'limit_export.json';
  removeFile(exportTarget);

  // the db and collection we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var testColl = testDB.data;

  // insert some data
  for (var i = 0; i < 50; i++) {
    testColl.insert({a: i});
  }
  // sanity check the insertion worked
  assert.eq(50, testColl.count());

  // export the data, using --limit
  var ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'data',
      '--sort', '{a:1}',
      '--limit', '20']
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

  // make sure the limit was applied to the export
  assert.eq(20, testColl.count());
  for (i = 0; i < 20; i++) {
    assert.eq(1, testColl.count({a: i}));
  }

  // success
  toolTest.stop();

}());
