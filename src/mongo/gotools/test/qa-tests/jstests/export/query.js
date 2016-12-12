(function() {

  if (typeof getToolTest === "undefined") {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests running mongoexport with --query specified.

  jsTest.log('Testing exporting with --query');

  var toolTest = getToolTest('query');
  var commonToolArgs = getCommonToolArguments();

  // the export target
  var exportTarget = 'query_export.json';
  removeFile(exportTarget);

  // the db and collections we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var sourceColl = testDB.source;
  var destColl = testDB.dest;

  // insert some data
  sourceColl.insert({a: 1, x: {b: '1'}});
  sourceColl.insert({a: 2, x: {b: '1', c: '2'}});
  sourceColl.insert({a: 1, c: '1'});
  sourceColl.insert({a: 2, c: '2'});
  // sanity check the insertion worked
  assert.eq(4, sourceColl.count());

  // export the data, with a query that will match nothing
  var ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--query', '{a:3}']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // import the data into the destination collection
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the export was blank
  assert.eq(0, destColl.count());

  // remove the export
  removeFile(exportTarget);

  // export the data, with a query matching a single element
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--query', "{a:1, c:'1'}"]
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // import the data into the destination collection
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the query was applied correctly
  assert.eq(1, destColl.count());
  assert.eq(1, destColl.count({a: 1, c: '1'}));

  // remove the export, clear the destination collection
  removeFile(exportTarget);
  destColl.remove({});

  // TOOLS-716 export the data, with a queryFile matching a single element
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--queryFile', "jstests/export/testdata/query.json"]
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // import the data into the destination collection
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the query was applied correctly
  assert.eq(1, destColl.count());
  assert.eq(1, destColl.count({a: 1, c: '1'}));

  // remove the export, clear the destination collection
  removeFile(exportTarget);
  destColl.remove({});


  // export the data, with a query on an embedded document
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--query', "{a:2, 'x.c':'2'}"]
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // import the data into the destination collection
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the query was applied correctly
  assert.eq(1, destColl.count());
  assert.eq(1, destColl.count({a: 2, "x.c": '2'}));

  // remove the export, clear the destination collection
  removeFile(exportTarget);
  destColl.remove({});

  // export the data, with a blank query (should match everything)
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--query', "{}"]
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // import the data into the destination collection
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the query was applied correctly
  assert.eq(4, destColl.count());

  // TOOLS-469 test queries containing extended JSON field (like dates)
  sourceColl.drop();
  destColl.drop();
  sourceColl.insert({
    a: 1,
    x: ISODate("2014-12-11T13:52:39.498Z"),
    y: ISODate("2014-12-13T13:52:39.498Z")
  });
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--query', '{x:{$gt:Date(1418305949498), $lt:Date(1418305979498)}, y:{$gt:{$date:1418478749498}, $lt:{$date:1418478769498}}}']
    .concat(commonToolArgs));
  assert.eq(0, ret);
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest']
    .concat(commonToolArgs));
  assert.eq(0, ret); assert.eq(1, destColl.count());

  // TOOLS-530 add support for ISODate and string formatting for query flag
  sourceColl.drop();
  destColl.drop();
  sourceColl.insert({
    a: 1,
    x: ISODate("2014-12-11T13:52:39.498Z"),
    y: ISODate("2014-12-13T13:52:39.498Z")
  });
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--query', '{x:{$gt:ISODate("2014-12-11T13:52:39.3Z"), $lt:ISODate("2014-12-11T13:52:39.5Z")}}']
    .concat(commonToolArgs));
  assert.eq(0, ret);
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest']
    .concat(commonToolArgs));
  assert.eq(0, ret);
  assert.eq(1, destColl.count());

  // success
  toolTest.stop();

}());
