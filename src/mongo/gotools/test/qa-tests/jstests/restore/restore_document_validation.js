/**
 * restore_document_validation.js
 *
 * This file test that mongorestore works with document validation. It both checks that when
 * validation is turned on invalid documents are not restored and that when a user indicates
 * they want to bypass validation, that all documents are restored.
 */

(function() {
  'use strict';
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  /**
   * Part 1: Test that restore follows document validation rules.
   */
  jsTest.log('Testing that restore reacts well to document validation');

  var toolTest = getToolTest('document_validation');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'doc_validation';
  resetDbpath(dumpTarget);

  // the db we will use
  var testDB = toolTest.db.getSiblingDB('test');

  // create 1000 documents, half of which will pass the validation
  for (var i = 0; i < 1000; i++) {
    if (i%2 === 0) {
      testDB.bar.insert({_id: i, num: i+1, s: String(i)});
    } else {
      testDB.bar.insert({_id: i, num: i+1, s: String(i), baz: i});
    }
  }
  // sanity check the insertion worked
  assert.eq(1000, testDB.bar.count(), 'all documents should be inserted');

  var ret = toolTest.runTool.apply(toolTest, ['dump', '-v']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret, 'dumping should run successfully');

  testDB.dropDatabase();
  assert.eq(0, testDB.bar.count(), 'after the drop, no documents should be seen');

  // sanity check that we can restore the data without validation
  ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  assert.eq(1000, testDB.bar.count(), 'after the restore, all documents should be seen');

  testDB.dropDatabase();
  assert.eq(0, testDB.bar.count(), 'after the drop, no documents should be seen');

  // turn on validation
  var r = testDB.createCollection('bar', {validator: {baz: {$exists: true}}});
  assert.eq(r, {ok: 1}, 'create collection with validation should work');

  // test that it's working
  r = testDB.bar.insert({num: 10000});
  assert.eq(r.nInserted, 0, "invalid documents shouldn't be inserted");

  // restore the 1000 records of which only 500 are valid
  ret = toolTest.runTool.apply(toolTest, ['restore', '-v']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret, 'restoring against a collection with validation on should still succeed');

  assert.eq(500, testDB.bar.count(), 'only the valid documents should be restored');

  /**
   * Part 2: Test that restore can bypass document validation rules.
   */
  jsTest.log('Testing that bypass document validation works');

  testDB.dropDatabase();

  // turn on validation
  r = testDB.createCollection('bar', {validator: {baz: {$exists: true}}});
  assert.eq(r, {ok: 1}, 'create collection with validation should work');

  // test that we cannot insert an 'invalid' document
  r = testDB.bar.insert({num: 10000});
  assert.eq(r.nInserted, 0, 'invalid documents should not be inserted');

  // restore the 1000 records again with bypassDocumentValidation turned on
  ret = toolTest.runTool.apply(toolTest, ['restore', '--bypassDocumentValidation']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret, 'restoring documents should work with bypass document validation set');
  assert.eq(1000, testDB.bar.count(),
              'all documents should be restored with bypass document validation set');

  /**
   * Part 3: Test that restore can restore the document validation rules,
   * if they're dumped with the collection.
   */
  jsTest.log('Testing that dump and restore restores the validation rules themselves');

  // clear out the database, including the validation rules
  testDB.dropDatabase();
  assert.eq(0, testDB.bar.count(), 'after the drop, no documents should be seen');

  // test that we can insert an 'invalid' document
  r = testDB.bar.insert({num: 10000});
  assert.eq(r.nInserted, 1,
              'invalid documents should be inserted after validation rules are dropped');

  testDB.dropDatabase();
  assert.eq(0, testDB.bar.count(), 'after the drop, no documents should be seen');

  // restore the 1000 records again
  ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);
  assert.eq(1000, testDB.bar.count());

  // turn on validation on a existing collection
  testDB.runCommand({'collMod': 'bar', 'validator': {baz: {$exists: true}}});

  // re-dump everything, this time dumping the validation rules themselves
  ret = toolTest.runTool.apply(toolTest, ['dump', '-v']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret, 'the dump should run successfully');

  // clear out the database, including the validation rules
  testDB.dropDatabase();
  assert.eq(0, testDB.bar.count(), 'after the drop, no documents should be seen');

  // test that we can insert an 'invalid' document
  r = testDB.bar.insert({num: 10000});
  assert.eq(r.nInserted, 1,
              'invalid documents should be inserted after we drop validation rules');

  testDB.dropDatabase();
  assert.eq(0, testDB.bar.count(), 'after the drop, no documents should be seen');

  // restore the 1000 records again
  ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret, 'restoring rules and some invalid documents should run successfully');
  assert.eq(500, testDB.bar.count(),
              'restoring the validation rules and documents should only restore valid documents');

  /**
   * Part 4: Test that restore can bypass the document validation rules,
   * even if they're dumped with the collection and restored with the collection.
   */
  jsTest.log('Testing that bypass document validation works when restoring the rules as well');

  // clear out the database, including the validation rules
  testDB.dropDatabase();
  assert.eq(0, testDB.bar.count(), 'after the drop, no documents should be seen');

  // test that we can insert an 'invalid' document
  r = testDB.bar.insert({num: 10000});
  assert.eq(r.nInserted, 1,
      'invalid documents should be inserted after validation rules are dropped');

  testDB.dropDatabase();
  assert.eq(0, testDB.bar.count(), 'after the drop, no documents should be seen');

  // restore the 1000 records again with bypassDocumentValidation turned on
  ret = toolTest.runTool.apply(toolTest, ['restore', '--bypassDocumentValidation']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret, 'restoring documents should work with bypass document validation set');
  assert.eq(1000, testDB.bar.count(),
      'all documents should be restored with bypass document validation set');
}());
