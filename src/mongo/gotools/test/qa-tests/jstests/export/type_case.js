(function() {

  if (typeof getToolTest === "undefined") {
    load('jstests/configs/plain_28.config.js');
  }

  // Testing exporting with various type specifiers

  jsTest.log('Testing exporting with various type specifiers');

  var toolTest = getToolTest('export_types');
  var commonToolArgs = getCommonToolArguments();

  // the db and collections we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var sourceColl = testDB.source;

  // the export target
  var exportTarget = 'type_export';

  // insert some data
  sourceColl.insert({a: 1});
  sourceColl.insert({a: 1, b: 1});
  sourceColl.insert({a: 1, b: 2, c: 3});
  // sanity check the insertion worked
  assert.eq(3, sourceColl.count());

  // first validate that invalid types are rejected
  var ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget,
      '--db', 'test',
      '--collection', 'source',
      '--type="foobar"',
      '--fields', 'a']
    .concat(commonToolArgs));
  assert.eq(3, ret);

  // create a dump file using a lowercase csv type
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget + ".csv",
      '--db', 'test',
      '--collection', 'source',
      '--type="csv"',
      '--fields', 'a']
    .concat(commonToolArgs));
  assert.eq(0, ret);
  var csvmd5 = md5sumFile(exportTarget + ".csv");

  // create a dump file using a uppercase csv type
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget + ".CSV",
      '--db', 'test',
      '--collection', 'source',
      '--type="CSV"',
      '--fields', 'a']
    .concat(commonToolArgs));
  var CSVmd5 = md5sumFile(exportTarget + ".CSV");
  // the files for the uppercase and lowercase types should match
  assert.eq(csvmd5, CSVmd5);

  // create a dump file using a mixedcase csv type
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget + ".cSv",
      '--db', 'test',
      '--collection', 'source',
      '--type="cSv"',
      '--fields', 'a']
    .concat(commonToolArgs));
  var cSvmd5 = md5sumFile(exportTarget + ".cSv");
  // the files for the uppercase and lowercase types should match
  assert.eq(csvmd5, cSvmd5);

  // then some json type tests

  // create a dump file using a lowercase json type
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget + ".json",
      '--db', 'test',
      '--collection', 'source',
      '--type="json"',
      '--fields', 'a']
    .concat(commonToolArgs));
  assert.eq(0, ret);
  var jsonmd5 = md5sumFile(exportTarget + ".json");

  // create a dump file using a uppercase json type
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget + ".JSON",
      '--db', 'test',
      '--collection', 'source',
      '--type="JSON"',
      '--fields', 'a']
    .concat(commonToolArgs));
  assert.eq(0, ret);
  var JSONmd5 = md5sumFile(exportTarget + ".JSON");

  // create a dump file using a uppercase blank (json) type
  ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', exportTarget + ".blank",
      '--db', 'test',
      '--collection', 'source',
      '--fields', 'a']
    .concat(commonToolArgs));
  assert.eq(0, ret);
  var blankmd5 = md5sumFile(exportTarget + ".blank");
  assert.eq(JSONmd5, jsonmd5);
  assert.eq(blankmd5, jsonmd5);

  // sanity check
  assert.neq(csvmd5, jsonmd5);

  // success
  toolTest.stop();

}());
