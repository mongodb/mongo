(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  jsTest.log('Testing importing a json file and checking types');

  var toolTest = getToolTest('import_types');

  // the import file
  var importFile = 'jstests/import/testdata/types.json';

  // the db and collection we'll use
  var testDB = toolTest.db.getSiblingDB('imported');
  var testColl = testDB.types;
  testColl.drop();
  var commonToolArgs = getCommonToolArguments();

  var importTypes = {
    "double_type": 1,
    "double_exponent_type": 1,
    "double_negative_type": 1,
    "NaN": 1,
    "infinity": 1,
    "negative_infinity": 1,
    "string_type": 2,
    "object_type": 3,
    "binary_data": 5,
    "undefined_type": 6,
    "object_id_type": 7,
    "true_type": 8,
    "false_type": 8,
    "date_type": 9,
    "iso_date_type": 9,
    "null_type": 10,
    "int32_type": 16,
    "int32_negative_type": 16,
    "number_int_type": 16,
    "int32_hex": 16,
    "int64_type": 18,
    "int64_negative_type": 18,
    "number_long_type": 18,
    "minkey_type": -1,
    "maxkey_type": 127,
    "regex_type": 11,
  };


  // import the data in from types.json
  ret = toolTest.runTool.apply(toolTest, ['import',
      '--file', importFile,
      '--db', 'imported',
      '--collection', 'types']
    .concat(commonToolArgs));
  assert.eq(0, ret);

  jsTest.log("Imported", importFile);

  var postImportDoc = testColl.findOne();
  printjson(postImportDoc);

  docKeys = Object.keys(importTypes);

  for (var i = 0; i < docKeys.length; i++) {
    jsTest.log("Checking type of", docKeys[i]);
    var typeNum = importTypes[docKeys[i]];
    var field = docKeys[i];
    var query = {};
    query[field] = {"$type": typeNum};
    printjson(query);
    assert.eq(testColl.find(query).count(), 1);
  }

  toolTest.stop();
}());
