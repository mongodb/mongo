(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests running mongoexport with different data types, and bringing it back
  // in with import.

  jsTest.log('Testing exporting, then importing, different data types');

  var toolTest = getToolTest('data_types');
  var commonToolArgs = getCommonToolArguments();

  // the export target
  var exportTarget = 'data_types_export.json';
  removeFile(exportTarget);

  // the db and collection we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var testColl = testDB.data;

  // insert some data, of different types
  testColl.insert({num: 1});
  testColl.insert({flt: 1.0});
  testColl.insert({str: '1'});
  testColl.insert({obj: {a: 1}});
  testColl.insert({arr: [0, 1]});
  testColl.insert({bd: new BinData(0, '1234')});
  testColl.insert({date: ISODate('2009-08-27T12:34:56.789')});
  testColl.insert({ts: new Timestamp(1234, 5678)});
  testColl.insert({rx: /foo*"bar"/i});
  // sanity check the insertion worked
  assert.eq(9, testColl.count());

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
  assert.eq(9, testColl.count());
  assert.eq(1, testColl.count({num: 1}));
  assert.eq(1, testColl.count({flt: 1.0}));
  assert.eq(1, testColl.count({str: '1'}));
  assert.eq(1, testColl.count({obj: {a: 1}}));
  assert.eq(1, testColl.count({arr: [0, 1]}));
  assert.eq(1, testColl.count({bd: new BinData(0, '1234')}));
  assert.eq(1, testColl.count({date: ISODate('2009-08-27T12:34:56.789')}));
  assert.eq(1, testColl.count({ts: new Timestamp(1234, 5678)}));
  assert.eq(1, testColl.count({rx: {$exists: true}}));

  // success
  toolTest.stop();

}());
