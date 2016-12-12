(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var toolTest = getToolTest('fields_json');
  var commonToolArgs = getCommonToolArguments();
  var testDB = toolTest.db.getSiblingDB('test');
  var sourceColl = testDB.source;

  // insert some data
  sourceColl.insert({a: 1});
  sourceColl.insert({a: 1, b: 1});
  sourceColl.insert({a: 1, b: 2, c: 3});

  // export it with pretty
  var ret = toolTest.runTool.apply(toolTest, ['export',
      '--out', "pretty.json",
      '--db', 'test',
      '--collection', 'source',
      '--pretty',
      '--jsonArray']
    .concat(commonToolArgs));
  assert.eq(0, ret);
  parsed = JSON.parse(cat('pretty.json'));
  assert.eq(parsed[0].a, 1);
  assert.eq(parsed[1].b, 1);
  assert.eq(parsed[2].b, 2);
  assert.eq(parsed[2].c, 3);

}());

