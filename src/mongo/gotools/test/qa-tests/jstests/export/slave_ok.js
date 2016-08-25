(function() {
  // Tests running mongoexport with --slaveOk.

  jsTest.log('Testing exporting with --slaveOk');

  // bring up a replica set with 3 nodes
  var replTest = new ReplSetTest({
    name: 'slave_ok',
    nodes: 3,
    oplogSize: 5,
    useHostName: true,
  });
  var nodes = replTest.startSet();
  replTest.initiate();
  replTest.awaitSecondaryNodes();

  // cache the primary
  var primary = replTest.getPrimary();

  // the export target
  var exportTarget = 'slave_ok_export.json';
  removeFile(exportTarget);

  // insert some data
  var testDB = primary.getDB('test');
  for (var i = 0; i < 10; i++) {
    testDB.data.insert({_id: i});
  }
  replTest.awaitReplication();

  // sanity check the insertion worked
  assert.eq(10, testDB.data.count());

  // make sure that exporting from any of the nodes works with --slaveOk
  nodes.forEach(function(node) {
    // remove the export, clean the destination collection
    removeFile(exportTarget);
    testDB.dest.remove({});
    printjson(replTest.status());

    var ret = runMongoProgram('mongoexport',
      '--db', 'test',
      '--collection', 'data',
      '--host', node.host,
      '--slaveOk',
      '--out', exportTarget);
    assert.eq(0, ret);

    ret = runMongoProgram('mongoimport',
      '--db', 'test',
      '--collection', 'dest',
      '--host', primary.host,
      '--file', exportTarget);
    assert.eq(0, ret);
    assert.eq(10, testDB.dest.count());
  });

  // success
  replTest.stopSet();

}());
