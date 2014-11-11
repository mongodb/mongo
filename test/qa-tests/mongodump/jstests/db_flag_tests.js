if (typeof getToolTest === 'undefined') {
  load('../plain_28.config.js');
}

(function() {
  var toolTest = getToolTest('dbFlagTest');
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

  // Running mongodump with `--db foo` should only dump the
  // 'foo' database, ignoring the 'baz' database
  var dumpArgs = ['dump', '--db', 'foo'].concat(commonToolArgs);
  toolTest.runTool.apply(toolTest, dumpArgs);
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
