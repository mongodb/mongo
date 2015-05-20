if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

(function() {
  var toolTest = getToolTest('outExtendedJsonFlagTest');
  var commonToolArgs = getCommonToolArguments();
  var db = toolTest.db.getSiblingDB('foo');

  var runDumpRestoreWithQuery = function(query) {
    resetDbpath('dump');
    var dumpArgs = ['dump', '--query', query, '--db', 'foo',
      '--collection', 'bar'].
          concat(getDumpTarget()).
          concat(commonToolArgs);
    assert.eq(toolTest.runTool.apply(toolTest, dumpArgs), 0,
      'mongodump should return exit status 0 when --db, --collection, and ' +
      '--query are all specified');

    db.dropDatabase();
    db.getSiblingDB('baz').dropDatabase();
    assert.eq(0, db.bar.count());
    assert.eq(0, db.getSiblingDB('baz').bar.count());

    var restoreArgs = ['restore'].
        concat(getRestoreTarget()).
        concat(commonToolArgs);
    assert.eq(toolTest.runTool.apply(toolTest, restoreArgs), 0,
      'mongorestore should succeed');
    resetDbpath('dump');
  };

  // '--query' should support extended JSON $date
  db.bar.drop();
  var d = new Date();
  db.bar.insert({ _id: 1, x: d });
  db.bar.insert({ _id: 2, x: new Date(2011, 8, 4) });
  runDumpRestoreWithQuery('{ x: { $date: ' + d.getTime() + ' } }');

  assert.eq(1, db.bar.count());
  assert.eq(1, db.bar.findOne()._id);

  // '--query' should support extended JSON $regex
  db.bar.drop();
  var d = new Date();
  db.bar.insert({ _id: 1, x: /bacon/i });
  db.bar.insert({ _id: 2, x: /bacon/ });
  runDumpRestoreWithQuery('{ x: { $regex: "bacon", $options: "i" } }');

  assert.eq(1, db.bar.count());
  assert.eq(1, db.bar.findOne()._id);

  // '--query' should support extended JSON $oid
  db.bar.drop();
  db.bar.insert({ x: 1 });
  db.bar.insert({ x: 2 });
  var doc = db.bar.findOne();

  runDumpRestoreWithQuery('{ _id: { $oid: "' + doc._id + '" } }');

  assert.eq(1, db.bar.count());
  assert.eq(db.bar.findOne()._id.toString(), doc._id.toString());

  // '--query' should support extended JSON $minKey
  db.bar.drop();
  db.bar.insert({ _id: 1, x: MinKey });
  db.bar.insert({ _id: 2, x: 1 });
  runDumpRestoreWithQuery('{ x: { $minKey: 1 } }');

  assert.eq(1, db.bar.count());
  assert.eq(1, db.bar.findOne()._id);

  // '--query' should support extended JSON $maxKey
  db.bar.drop();
  db.bar.insert({ _id: 1, x: MaxKey });
  db.bar.insert({ _id: 2, 
    x: 1 });
  runDumpRestoreWithQuery('{ x: { $maxKey: 1 } }');

  assert.eq(1, db.bar.count());
  assert.eq(1, db.bar.findOne()._id);

  toolTest.stop();
})();
