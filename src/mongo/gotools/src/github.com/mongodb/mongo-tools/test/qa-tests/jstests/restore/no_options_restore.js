(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Using the collection options command is the way to get full
  // collection options as of 2.8, so we use this helper to
  // pull the options from a listCollections cursor.
  var extractCollectionOptions = function(db, name) {
    var res = db.runCommand("listCollections");
    for (var i = 0; i < res.cursor.firstBatch.length; i++) {
      if (res.cursor.firstBatch[i].name === name) {
        return res.cursor.firstBatch[i].options;
      }
    }
    return {};
  };

  // Tests that running mongorestore with --noOptionsRestore does
  // not restore collection options, and that running it without
  // --noOptionsRestore does restore collection options.
  jsTest.log('Testing restoration with --noOptionsRestore');

  var toolTest = getToolTest('no_options_restore');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'no_options_restore_dump';
  resetDbpath(dumpTarget);

  // the db we'll use
  var testDB = toolTest.db.getSiblingDB('test');

  // we'll use three different collections - the first will have
  // options set, the second won't, the third will be capped.
  // TODO: why aren't these being used?
  // var collWithOptions = testDB.withOptions;
  // var collWithoutOptions = testDB.withoutOptions;
  // var collCapped = testDB.capped;

  // create the noPadding collection
  var noPaddingOptions = {noPadding: true};
  testDB.createCollection('withOptions', noPaddingOptions);

  // create the capped collection
  var cappedOptions = {capped: true, size: 4096, autoIndexId: true};
  testDB.createCollection('capped', cappedOptions);

  // insert some data into all three collections
  ['withOptions', 'withoutOptions', 'capped'].forEach(function(collName) {
    var data = [];
    for (var i = 0; i < 50; i++) {
      data.push({_id: i});
    }
    testDB[collName].insertMany(data);
    // sanity check the insertions worked
    assert.eq(50, testDB[collName].count());
  });

  // add options to the appropriate collection
  cmdRet = testDB.runCommand({'collMod': 'withOptions', usePowerOf2Sizes: true});
  assert.eq(1, cmdRet.ok);

  // store the default options, because they change based on storage engine
  var baseCappedOptionsFromDB = extractCollectionOptions(testDB, 'capped');
  var baseWithOptionsFromDB = extractCollectionOptions(testDB, 'withOptions');
  var baseWithoutOptionsFromDB = extractCollectionOptions(testDB, 'withoutOptions');

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // drop the data
  testDB.dropDatabase();

  // restore the data
  ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored correctly
  ['withOptions', 'withoutOptions', 'capped'].forEach(function(collName) {
    assert.eq(50, testDB[collName].count());
  });

  // make sure the options were restored correctly
  var cappedOptionsFromDB = extractCollectionOptions(testDB, 'capped');
  // Restore no longer honors autoIndexId.
  if (!cappedOptionsFromDB.hasOwnProperty('autoIndexId')) {
    cappedOptionsFromDB.autoIndexId = true;
  }
  assert.eq(baseCappedOptionsFromDB, cappedOptionsFromDB);
  var withOptionsFromDB = extractCollectionOptions(testDB, 'withOptions');
  assert.eq(baseWithOptionsFromDB, withOptionsFromDB);
  var withoutOptionsFromDB = extractCollectionOptions(testDB, 'withoutOptions');
  assert.eq(baseWithoutOptionsFromDB, withoutOptionsFromDB);

  // drop the data
  testDB.dropDatabase();

  // restore the data, without the options
  ret = toolTest.runTool.apply(toolTest, ['restore', '--noOptionsRestore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored correctly
  ['withOptions', 'withoutOptions', 'capped'].forEach(function(collName) {
    assert.eq(50, testDB[collName].count());
  });

  // make sure the options were not restored
  cappedOptionsFromDB = extractCollectionOptions(testDB, 'capped');
  assert.eq(baseWithoutOptionsFromDB, cappedOptionsFromDB);
  withOptionsFromDB = extractCollectionOptions(testDB, 'withOptions');
  assert.eq(baseWithoutOptionsFromDB, withOptionsFromDB);
  withoutOptionsFromDB = extractCollectionOptions(testDB, 'withoutOptions');
  assert.eq(baseWithoutOptionsFromDB, withoutOptionsFromDB);

  // additional check that the capped collection is no longer capped
  var cappedStats = testDB.capped.stats();
  assert(!cappedStats.capped);

  // success
  toolTest.stop();

}());
