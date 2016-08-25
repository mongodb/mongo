/**
 * boolean_type.js
 *
 * This file tests the Boolean() type in mongoimport. Importing a document with a field like
 * Boolean(1) should be treated identically to how the shell would insert a similar document.
 */

(function() {
  'use strict';
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  jsTest.log('Testing running import with various options in the Boolean() type');

  var toolTest = getToolTest('import');
  var db1 = toolTest.db;
  var commonToolArgs = getCommonToolArguments();
  var testDocs = [
	{key: 'a', bool: Boolean(1)},
	{key: 'b', bool: Boolean(0)},
	{key: 'c', bool: Boolean(140)},
	{key: 'd', bool: Boolean(-140.5)},
	{key: 'e', bool: Boolean(Boolean(1))},
	{key: 'f', bool: Boolean(Boolean(0))},
	{key: 'g', bool: Boolean('')},
	{key: 'h', bool: Boolean('f')},
	{key: 'i', bool: Boolean(null)},
	{key: 'j', bool: Boolean(undefined)},
	{key: 'k', bool: Boolean(true)},
	{key: 'l', bool: Boolean(false)},
	{key: 'm', bool: Boolean(true, false)},
	{key: 'n', bool: Boolean(false, true)},
	{key: 'o', bool: [Boolean(1), Boolean(0), Date(23)]},
	{key: 'p', bool: Boolean(Date(15))},
	{key: 'q', bool: Boolean(0x585)},
	{key: 'r', bool: Boolean(0x0)},
	{key: 's', bool: Boolean()},
  ];

  var ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', 'jstests/import/testdata/boolean.json',
      '--db', 'imported',
      '--collection', 'testcollbool']
    .concat(commonToolArgs));
  assert.eq(ret, 0);

  // Confirm that mongoimport imports the testDocs identically to how the shell interprets them.
  var coll = db1.getSiblingDB('imported').testcollbool;
  for (var i = 0; i < testDocs.length; i++) {
    var postImportDoc = coll.findOne({key: testDocs[i].key});
    assert.eq(testDocs[i].key, postImportDoc.key,
        'imported doc ' + testDocs[i].key + 'does not match original');
  }

  toolTest.stop();
}());
