/**
 * output_file.js
 *
 * This file tests outputting bsondump to a file when the input is from a file.
 */

(function() {
  'use strict';

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var toolTest = getToolTest('bson_output_file');
  var commonToolArgs = getCommonToolArguments();

  // The db and collections we'll use.
  var testDB = toolTest.db.getSiblingDB('test');
  var destColl = testDB.bsondump;

  // Test using a flag to specify the output file..
  var exportTarget = 'bson_dump.json';
  removeFile(exportTarget);

  var ret = _runMongoProgram("bsondump",
    "--type=json",
    "--bsonFile", "jstests/bson/testdata/sample.bson",
    "--outFile", exportTarget);
  assert.eq(ret, 0, "bsondump should exit successfully with 0");

  // Import the data into the destination collection to check correctness.
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'bsondump',
      '--type', 'json']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // Make sure everything was dumped.
  assert.eq(1, destColl.count({a: 1.0}));
  assert.eq(1, destColl.count({a: 2.5}));
  assert.eq(1, destColl.count({a: 4.0}));
  assert.eq(1, destColl.count({a: 4.01}));


  // Test using a positional argument to specify the output file.
  removeFile(exportTarget);

  ret = _runMongoProgram("bsondump",
    "--type=json",
    "--outFile", exportTarget,
    "jstests/bson/testdata/sample.bson");
  assert.eq(ret, 0, "bsondump should exit successfully with 0");

  // Import the data into the destination collection to check correctness.
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', exportTarget,
      '--db', 'test',
      '--collection', 'bsondump',
      '--type', 'json']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // Make sure everything was dumped.
  assert.eq(1, destColl.count({a: 1.0}));
  assert.eq(1, destColl.count({a: 2.5}));
  assert.eq(1, destColl.count({a: 4.0}));
  assert.eq(1, destColl.count({a: 4.01}));

}());
