(function() {

  if (typeof getToolTest === "undefined") {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests running mongoexport exporting to csv using the --fieldFile option
  jsTest.log('Testing exporting to csv using the --fieldFile option');

  var toolTest = getToolTest('field_file');
  var commonToolArgs = getCommonToolArguments();

  // the db and collections we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var sourceColl = testDB.source;
  var destColl = testDB.dest;

  // the export target
  var exportTarget = 'jstests/export/testdata/field_file_export.csv';
  removeFile(exportTarget);

  // insert some data
  sourceColl.insert({a: 1});
  sourceColl.insert({a: 1, b: 1});
  sourceColl.insert({a: 1, b: 2, c: 3});
  // sanity check the insertion worked
  assert.eq(3, sourceColl.count());

  // export the data, using a field file that specifies 'a' and 'b'
  var ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--type=csv',
      '--fieldFile', 'jstests/export/testdata/simple_field_file']
    .concat(commonToolArgs));
  assert.eq(0, ret);


  // import the data into the destination collection
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest',
      '--type=csv',
      '--fields', 'a,b,c']
    .concat(commonToolArgs));
  assert.eq(0, ret);


  // make sure only the specified fields were exported
  assert.eq(3, destColl.count({a: 1}));
  assert.eq(1, destColl.count({b: 1}));
  assert.eq(1, destColl.count({b: 2}));
  assert.eq(0, destColl.count({c: 3}));

  // success
  toolTest.stop();

}());
