(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  if (dump_targets !== "standard") {
    print('skipping test incompatable with archiving or compression');
    return assert(true);
  }

  // Tests that using mongorestore on a collection with XXX

  jsTest.log('Testing that restoration of XXX.');

  var toolTest = getToolTest('ordered_partial_index');
  var commonToolArgs = getCommonToolArguments();
  var testDB = toolTest.db.getSiblingDB('test');
  assert.eq(testDB.foo.exists(), null, "collection already exists in db");

  // run a restore against the mongos
  var ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget('jstests/restore/testdata/dump_ordered_partial_index'))
    .concat(commonToolArgs));
  assert.eq(0, ret, "the restore does not crash");

  var apfe;
  var indexes = testDB.foo.getIndexes();
  for (var i=0; i<indexes.length; i++) {
    if (indexes[i].name==="apfe") {
      apfe=indexes[i].partialFilterExpression;
    }
  }

  i = 0;
  for (var fe in apfe) {
    if (apfe.hasOwnProperty(fe)) {
      assert.eq(fe, "a"+i, "partial indexes are in the correct order");
      i++;
    } else {
      doassert("No property " + fe);
    }
  }

}());
