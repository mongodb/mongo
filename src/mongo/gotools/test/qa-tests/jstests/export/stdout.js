// Tests running mongoexport writing to stdout.
(function() {
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  jsTest.log('Testing exporting to stdout');

  var toolTest = new ToolTest('stdout');
  toolTest.startDB('foo');

  // the db and collection we'll use
  var testDB = toolTest.db.getSiblingDB('test');
  var testColl = testDB.data;

  // insert some data
  for (var i = 0; i < 20; i++) {
    testColl.insert({_id: i});
  }
  // sanity check the insertion worked
  assert.eq(20, testColl.count());

  // export the data, writing to stdout
  var ret = toolTest.runTool('export', '--db', 'test', '--collection', 'data');
  assert.eq(0, ret);

  // wait for full output to appear
  assert.strContains.soon('exported 20 records', rawMongoProgramOutput,
      'should show number of exported records');

  // grab the raw output
  var output = rawMongoProgramOutput();

  // make sure it contains the json output
  for (i = 0; i < 20; i++) {
    assert.neq(-1, output.indexOf('{"_id":'+i+'.0}'));
  }

  // success
  toolTest.stop();
}());
