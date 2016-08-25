(function() {

  if (typeof getToolTest === "undefined") {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests running mongoexport with --sort and --skip specified.

  jsTest.log('Testing exporting with --sort and --skip');

  var toolTest = getToolTest('sort_and_skip');
  var commonToolArgs = getCommonToolArguments();

  // the export target
  var exportTarget = 'sort_and_skip_export.json';
  removeFile(exportTarget);

  // the db and collection we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var testColl = testDB.data;

  // insert some data, in a different order than we'll be sorting it
  for (var i = 30; i > 20; i--) {
    testColl.insert({a: i});
  }
  for (i = 31; i < 50; i++) {
    testColl.insert({a: i});
  }
  for (i = 20; i >= 0; i--) {
    testColl.insert({a: i});
  }
  // sanity check the insertion worked
  assert.eq(50, testColl.count());

  // export the data, using --skip
  var ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'data',
      '--sort', '{a:1}',
      '--skip', '20']
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

  // make sure the skip was applied to the export, and that
  // the sort functioned so that the correct documents
  // were skipped
  assert.eq(30, testColl.count());
  for (i = 20; i < 50; i++) {
    assert.eq(1, testColl.count({a: i}));
  }

  // success
  toolTest.stop();

}());
