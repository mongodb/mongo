if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

(function() {
  resetDbpath('dump');
  var toolTest = getToolTest('outFlagTest');
  var commonToolArgs = getCommonToolArguments();
  var db = toolTest.db.getSiblingDB('foo');

  db.dropDatabase();
  assert.eq(0, db.bar.count());
  db.getSiblingDB('baz').dropDatabase();
  assert.eq(0, db.getSiblingDB('baz').bar.count());

  // Insert into the 'foo' database
  db.bar.insert({ x: 1 });
  db.bar.insert({ x: 0 });
  // and into the 'baz' database
  db.getSiblingDB('baz').bar.insert({ x: 2 });

    // Running mongodump with '--query' specified but no '--db' should fail
  var dumpArgs = ['dump', '--collection', 'bar',
    '--query', '"{ x: { $gt:0 } }"'].concat(commonToolArgs);
  assert(toolTest.runTool.apply(toolTest, dumpArgs) !== 0,
    'mongodump should exit with a non-zero status when --query is ' +
    'specified but --db isn\'t');

  // Running mongodump with '--query' specified but no '--collection' should
  // fail
  var dumpArgs = ['dump', '--db', 'foo', '--query', '"{ x: { $gt:0 } }"'].
    concat(commonToolArgs);
  assert(toolTest.runTool.apply(toolTest, dumpArgs) !== 0,
    'mongodump should exit with a non-zero status when --query is ' +
    'specified but --collection isn\'t');

  // Running mongodump with '--query' should only get matching documents
  resetDbpath('dump');
  var dumpArgs = ['dump', '--query', '{ x: { $gt:0 } }', '--db', 'foo',
    '--collection', 'bar'].concat(commonToolArgs);
  assert.eq(toolTest.runTool.apply(toolTest, dumpArgs), 0,
    'mongodump should return exit status 0 when --db, --collection, and ' +
    '--query are all specified');

  db.dropDatabase();
  db.getSiblingDB('baz').dropDatabase();
  assert.eq(0, db.bar.count());
  assert.eq(0, db.getSiblingDB('baz').bar.count());

  var restoreArgs = ['restore'].concat(commonToolArgs);
  assert.eq(toolTest.runTool.apply(toolTest, restoreArgs), 0,
    'mongorestore should succeed');
  assert.eq(1, db.bar.count());
  assert.eq(0, db.getSiblingDB('baz').bar.count());

  toolTest.stop();
})();
