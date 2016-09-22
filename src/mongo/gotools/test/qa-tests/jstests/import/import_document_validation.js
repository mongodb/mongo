/**
 * import_document_validation.js
 *
 * This file test that mongoimport works with document validation. It both checks that when
 * validation is turned on invalid documents are not imported and that when a user indicates
 * they want to bypass validation, that all documents are imported.
 */
(function() {
  'use strict';
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  /**
   * Part 1: Test that import follows document validation rules.
   */
  jsTest.log('Testing that import reacts well to document validation');

  var toolTest = getToolTest('import_document_validation');
  var commonToolArgs = getCommonToolArguments();

  // the db we will use
  var testDB = toolTest.db.getSiblingDB('test');

  // create 1000 documents, half of which will pass the validation
  var data = [];
  for (var i = 0; i < 1000; i++) {
    if (i%2 === 0) {
      data.push({_id: i, num: i+1, s: '' + i});
    } else {
      data.push({_id: i, num: i+1, s: '' + i, baz: i});
    }
  }
  testDB.bar.insertMany(data);
  // sanity check the insertion worked
  assert.eq(1000, testDB.bar.count());

  // export the data
  var ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', toolTest.extFile,
      '-d', 'test',
      '-c', 'bar']
    .concat(commonToolArgs));
  assert.eq(0, ret, 'export should run successfully');

  testDB.dropDatabase();
  assert.eq(0, testDB.bar.count(),
      'after dropping the database, no documents should be seen');

  // sanity check that we can import the data without validation
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', toolTest.extFile,
      '--db', 'test',
      '-c', 'bar']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  assert.eq(1000, testDB.bar.count(),
      'after import, the documents should be seen again');

  testDB.dropDatabase();
  assert.eq(0, testDB.bar.count(),
      'after dropping the database, no documents should be seen');

  // turn on validation
  var r = testDB.createCollection('bar', {validator: {baz: {$exists: true}}});
  assert.eq(r, {ok: 1}, 'create collection with validation works');

  // test that it's working
  r = testDB.bar.insert({num: 10000});
  assert.eq(r.nInserted, 0, "invalid documents shouldn't be inserted");

  // import the 1000 records of which only 500 are valid
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', toolTest.extFile,
      '--db', 'test',
      '-c', 'bar']
    .concat(commonToolArgs));
  assert.eq(0, ret,
      'import against a collection with validation on still succeeds');

  assert.eq(500, testDB.bar.count(), 'only the valid documents are imported');

  /**
   * Part 2: Test that import can bypass document validation rules.
   */
  jsTest.log('Testing that bypass document validation works');

  testDB.dropDatabase();

  // turn on validation
  r = testDB.createCollection('bar', {validator: {baz: {$exists: true}}});
  assert.eq(r, {ok: 1}, 'create collection with validation should work');

  // test that we cannot insert an 'invalid' document
  r = testDB.bar.insert({num: 10000});
  assert.eq(r.nInserted, 0, 'invalid documents should not be inserted');

  // import the 1000 records again with bypassDocumentValidation turned on
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', toolTest.extFile,
      '--db', 'test',
      '-c', 'bar',
      '--bypassDocumentValidation']
    .concat(commonToolArgs));
  assert.eq(0, ret,
      'importing documents should work with bypass document validation set');
  assert.eq(1000, testDB.bar.count(),
      'all documents should be imported with bypass document validation set');
}());
