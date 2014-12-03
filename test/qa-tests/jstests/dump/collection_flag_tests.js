if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

(function() {
  resetDbpath('dump');
  var toolTest = getToolTest('collectionFlagTest');
  var commonToolArgs = getCommonToolArguments();
  var db = toolTest.db.getSiblingDB('foo');

  db.dropDatabase();
  assert.eq(0, db.bar.count());
  db.getSiblingDB('baz').dropDatabase();
  assert.eq(0, db.getSiblingDB('baz').bar.count());

  // Insert into the 'foo' database
  db.bar.insert({ x: 1 });
  // and into the 'baz' database
  db.getSiblingDB('baz').bar.insert({ x: 2 });

  // Running mongodump with `--collection bar` and no '--db' flag should throw
  // an error
  var dumpArgs = ['dump', '--collection', 'bar'].concat(commonToolArgs);
  assert(toolTest.runTool.apply(toolTest, dumpArgs) !== 0,
    'mongodump should exit with a non-zero status when --collection is ' +
    'specified but --db isn\'t');

  // Running mongodump with `--collection bar --db foo` should only dump
  // the 'foo' database and ignore the 'baz' database
  resetDbpath('dump');
  var dumpArgs = ['dump', '--collection', 'bar', '--db', 'foo'].
    concat(commonToolArgs);
  assert.eq(toolTest.runTool.apply(toolTest, dumpArgs), 0,
    'mongodump should succeed when both --collection and --db are specified');
  db.dropDatabase();
  db.getSiblingDB('baz').dropDatabase();
  assert.eq(0, db.bar.count());
  assert.eq(0, db.getSiblingDB('baz').bar.count());

  var restoreArgs = ['restore'].concat(commonToolArgs);
  toolTest.runTool.apply(toolTest, restoreArgs);
  assert.eq(1, db.bar.count());
  assert.eq(0, db.getSiblingDB('baz').bar.count());

  toolTest.stop();
})();
