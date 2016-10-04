(function() {

  if (typeof getToolTest === "undefined") {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests exporting nested fields to csv.

  jsTest.log('Testing exporting nested fields to csv');

  var toolTest = getToolTest('nested_fields_csv');
  var commonToolArgs = getCommonToolArguments();

  // the db and collections we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var sourceColl = testDB.source;
  var destColl = testDB.dest;

  // the export target
  var exportTarget = 'nested_fields_export.csv';
  removeFile(exportTarget);

  // insert some data
  sourceColl.insert({a: 1});
  sourceColl.insert({a: 2, b: {c: 2}});
  sourceColl.insert({a: 3, b: {c: 3, d: {e: 3}}});
  sourceColl.insert({a: 4, x: null});
  // sanity check the insertion worked
  assert.eq(4, sourceColl.count());

  // export the data, specifying nested fields to export
  var ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--csv',
      '--fields', 'a,b.d.e,x.y']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // import the data
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest',
      '--type', 'csv',
      '--headerline']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure that the non-specified fields were ignored, and the
  // specified fields were added correctly
  assert.eq(0, destColl.count({'b.c': 2}));
  assert.eq(0, destColl.count({'b.c': 3}));
  assert.eq(1, destColl.count({'b.d.e': 3}));
  assert.eq(3, destColl.count({'b.d.e': ''}));
  assert.eq(1, destColl.count({a: 1}));
  assert.eq(1, destColl.count({a: 2}));
  assert.eq(1, destColl.count({a: 3}));
  assert.eq(4, destColl.count({'x.y': ''}));

  // success
  toolTest.stop();

}());
