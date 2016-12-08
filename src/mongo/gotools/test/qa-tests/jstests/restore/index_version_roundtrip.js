(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests that mongorestore correctly round-trips _id index versions.

  jsTest.log('Testing restoration of different types of indexes');

  var toolTest = getToolTest('index_version_roundtrip');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var name = 'idx_version_rt_dump';
  resetDbpath(name);

  var testDB = toolTest.db.getSiblingDB(name);

  // drop the db
  testDB.dropDatabase();

  assert.commandWorked(testDB.runCommand({
    create: "coll1",
    idIndex: {
      v: 1,
      key: {
        _id: 1
      },
      name: "_id_",
      ns: name + ".coll1",
    }
  }));
  assert.commandWorked(testDB.runCommand({
    create: "coll2",
    idIndex: {
      v: 2,
      key: {
        _id: 1
      },
      name: "_id_",
      ns: name + ".coll2",
    }
  }));

  // create an aditional index to verify non _id indexes work
  assert.commandWorked(testDB.coll1.ensureIndex({a: 1}, {v: 1}));
  assert.commandWorked(testDB.coll2.ensureIndex({a: 1}, {v: 2}));

  // insert arbitrary data so the collections aren't empty
  testDB.coll1.insert({a: 123});
  testDB.coll2.insert({a: 123});

  // store the index specs, for comparison after dump / restore
  var idxSorter = function(a, b) {
    return a.name.localeCompare(b.name);
  };

  var idxPre1 = testDB.coll1.getIndexSpecs();
  idxPre1.sort(idxSorter);
  var idxPre2 = testDB.coll2.getIndexSpecs();
  idxPre2.sort(idxSorter);

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(name))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // drop the db
  testDB.dropDatabase();
  // sanity check that the drop worked
  assert.eq(0, db.runCommand({
    listCollections: 1
  }).cursor.firstBatch.length);

  // restore the data
  ret = toolTest.runTool.apply(toolTest, ['restore', '--keepIndexVersion']
    .concat(getRestoreTarget(name))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored
  assert.eq(1, testDB.coll1.find().itcount());
  assert.eq(1, testDB.coll2.find().itcount());

  // make sure the indexes were restored correctly
  var idxPost1 = testDB.coll1.getIndexSpecs();
  idxPost1.sort(idxSorter);
  assert.eq(idxPre1.length, idxPost1.length,
    "indexes before: " + tojson(idxPre1) + "\nindexes after: " + tojson(idxPost1));
  for (var i = 0; i < idxPre1.length; i++) {
    assert.eq(idxPre1[i], idxPost1[i]);
  }

  var idxPost2 = testDB.coll2.getIndexSpecs();
  idxPost2.sort(idxSorter);
  assert.eq(idxPre2.length, idxPost2.length,
    "indexes before: " + tojson(idxPre2) + "\nindexes after: " + tojson(idxPost2));
  for (i = 0; i < idxPre2.length; i++) {
    assert.eq(idxPre2[i], idxPost2[i]);
  }

  // success
  toolTest.stop();
}());
