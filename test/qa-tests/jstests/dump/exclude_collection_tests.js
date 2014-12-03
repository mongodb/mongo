if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

(function() {
  resetDbpath('dump');
  var toolTest = getToolTest('excludeCollectionFlagTest');
  var commonToolArgs = getCommonToolArguments();
  var db = toolTest.db.getSiblingDB('foo');

  db.dropDatabase();
  assert.eq(0, db.bar.count());

  db.bar.insert({ x: 1 });
  db.baz.insert({ x: 2 });

  // Specifying both --excludeCollection and --collection should fail
  var dumpArgs = ['dump', '--db', 'foo', '--collection', 'baz',
    '--excludeCollection', 'baz'].concat(commonToolArgs);
  assert(toolTest.runTool.apply(toolTest, dumpArgs) !== 0,
    'mongodump should fail if both --collection and --excludeCollection ' +
    'specified');

  // --excludeCollection without --db should fail
  dumpArgs = ['dump', '--excludeCollection', 'baz'].concat(commonToolArgs);
  assert(toolTest.runTool.apply(toolTest, dumpArgs) !== 0,
    'mongodump should fail if --excludeCollection is specified but not --db');

  // If --db and --excludeCollection are specified, should dump all collections
  // except for the one specified in excludeCollection.
  dumpArgs = ['dump', '--db', 'foo',
    '--excludeCollection', 'baz'].concat(commonToolArgs);
  assert.eq(toolTest.runTool.apply(toolTest, dumpArgs), 0,
    'mongodump with --excludeCollection should succeed');
  db.dropDatabase();
  assert.eq(0, db.bar.count());
  assert.eq(0, db.baz.count());

  var restoreArgs = ['restore'].concat(commonToolArgs);
  assert.eq(toolTest.runTool.apply(toolTest, restoreArgs), 0,
    'mongorestore should succeed');
  assert.eq(1, db.bar.count());
  assert.eq(0, db.baz.count());

  toolTest.stop();
})();
