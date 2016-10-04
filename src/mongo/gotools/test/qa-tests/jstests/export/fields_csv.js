(function() {

  if (typeof getToolTest === "undefined") {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests running mongoexport exporting to csv using the --fields option.

  jsTest.log('Testing exporting to csv using the --fields option');

  var toolTest = getToolTest('fields_csv');
  var commonToolArgs = getCommonToolArguments();

  // the db and collections we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var sourceColl = testDB.source;
  var destColl = testDB.dest;

  // the export target
  var exportTarget = 'fields_export.csv';
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
      '--csv',
      '--fields', 'a']
    .concat(commonToolArgs));
  assert.eq(0, ret);

    // import the data into the destination collection
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest',
      '--type', 'csv',
      '--fields', 'a,b,c']
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
      '--csv',
      '--fields', 'a,b,c']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // import the data into the destination collection
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'dest',
      '--type', 'csv',
      '--fields', 'a,b,c']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure everything was exported
  assert.eq(3, destColl.count({a: 1}));
  assert.eq(1, destColl.count({b: 1}));
  assert.eq(1, destColl.count({b: 2}));
  assert.eq(1, destColl.count({c: 3}));

  // make sure the _id was NOT exported - the _id for the
  // corresponding documents in the two collections should
  // be different
  var fromSource = sourceColl.findOne({a: 1, b: 1});
  var fromDest = destColl.findOne({a: 1, b: 1});
  assert.neq(fromSource._id, fromDest._id);


  /* Test passing positional arguments to --fields */

  // outputMatchesExpected takes an output string and returns
  // a boolean indicating if any line of the output matched
  // the expected string.
  var outputMatchesExpected = function(output, expected) {
    var found = false;
    output.split('\n').forEach(function(line) {
      if (line.match(expected)) {
        found = true;
      }
    });
    return found;
  };

  // remove the export, clear the destination collection
  removeFile(exportTarget);
  sourceColl.remove({});

  // ensure source collection is empty
  assert.eq(0, sourceColl.count());

  // insert some data
  sourceColl.insert({a: [1, 2, 3, 4, 5], b: {c: [-1, -2, -3, -4]}});
  sourceColl.insert({a: 1, b: 2, c: 3, d: {e: [4, 5, 6]}});
  sourceColl.insert({a: 1, b: 2, c: 3, d: 5, e: {"0": ["foo", "bar", "baz"]}});
  sourceColl.insert({a: 1, b: 2, c: 3, d: [4, 5, 6], e: [{"0": 0, "1": 1}, {"2": 2, "3": 3}]});

  // ensure the insertion worked
  assert.eq(4, sourceColl.count());

  // use the following fields as filters:
  var cases = [
      {field: 'd.e.2', expected: /6/}, // specify nested field with array value
      {field: 'e.0.0', expected: /foo/}, // specify nested field with numeric array value
      {field: 'b,d.1,e.1.3', expected: /2,5,3/}, // specify varying levels of field nesting
  ];

  var output;

  for (var i = 0; i < cases.length; i++) {
    ret = toolTest.runTool.apply(toolTest, ['export',
        '--fields', cases[i].field,
        '--out', exportTarget,
        '--db', 'test',
        '--collection', 'source',
        '--csv']
      .concat(commonToolArgs));
    assert.eq(0, ret);
    output = cat(exportTarget);
    jsTest.log("Fields Test " + (i + 1) + ": \n" + output);
    assert.eq(outputMatchesExpected(output, cases[i].expected), true);
  }

  // test with $ projection and query
  cases = [
      {query: '{ d: 4 }', field: 'd.$', expected: /[4]/},
      {query: '{ a: { $gt: 1 } }', field: 'a.$', expected: /[2]/},
      {query: '{ "b.c": -1 }', field: 'b.c.$', expected: /[-1]/},
  ];

  for (i = 0; i < cases.length; i++) {
    ret = toolTest.runTool.apply(toolTest, ['export',
        '--query', cases[i].query,
        '--fields', cases[i].field,
        '--out', exportTarget,
        '--db', 'test',
        '--collection', 'source',
        '--csv']
      .concat(commonToolArgs));
    assert.eq(0, ret);
    output = cat(exportTarget);
    jsTest.log("Fields + Query Test " + (i + 1) + ": \n" + output);
    assert.eq(outputMatchesExpected(output, cases[i].expected), true);
  }

  // success
  toolTest.stop();

}());
