(function() {

    if (typeof getToolTest === 'undefined') {
        load('jstests/configs/plain_28.config.js');
    }

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

    // turn off auto-use of power of 2 sizes, so we know exactly what
    // options new collections will have
    var cmdRet = testDB.adminCommand({ setParameter: 1, 
        newCollectionsUsePowerOf2Sizes: false }); 
    assert.eq(1, cmdRet.ok);

    // we'll use three different collections - the first will have
    // options set, the second won't, the third will be capped.
    var collWithOptions = testDB.withOptions;
    var collWithoutOptions = testDB.withoutOptions;
    var collCapped = testDB.capped;

    // create the capped collection
    var cappedOptions = { capped: true, size: 4096, autoIndexId: true };
    testDB.createCollection('capped', cappedOptions);

    // insert some data into all three collections
    ['withOptions', 'withoutOptions', 'capped'].forEach(function(collName) {
        for (var i = 0; i < 50; i++) {
            testDB[collName].insert({_id: i});
        }
        // sanity check the insertions worked
        assert.eq(50, testDB[collName].count());
    });

    // add options to the appropriate collection
    cmdRet = testDB.runCommand({ 'collMod': 'withOptions', usePowerOf2Sizes: true });
    assert.eq(1, cmdRet.ok);
    
    // dump the data
    var ret = toolTest.runTool.apply(
            toolTest,
            ['dump', '--out', dumpTarget].
                concat(commonToolArgs)
    ); 
    assert.eq(0, ret);

    // drop the data
    testDB.dropDatabase();

    // restore the data
    ret = toolTest.runTool.apply(
            toolTest,
            ['restore', dumpTarget].
                concat(commonToolArgs)
    );
    assert.eq(0, ret);

    // make sure the data was restored correctly
    ['withOptions', 'withoutOptions', 'capped'].forEach(function(collName) {
        assert.eq(50, testDB[collName].count());
    });

    // make sure the options were restored correctly
    var cappedOptionsFromDB = testDB['system.namespaces'].findOne({ name: 'test.capped' });
    assert.eq(cappedOptions, cappedOptionsFromDB.options);
    var withOptionsFromDB = testDB['system.namespaces'].findOne({ name: 'test.withOptions' });
    assert.eq({ flags: 1 }, withOptionsFromDB.options);
    var withoutOptionsFromDB = testDB['system.namespaces'].findOne({ name: 'test.withoutOptions' });
    assert.eq(undefined, withoutOptionsFromDB.options);

    // drop the data
    testDB.dropDatabase();

    // restore the data, without the options
    ret = toolTest.runTool.apply(
            toolTest,
            ['restore', '--noOptionsRestore', dumpTarget].
                concat(commonToolArgs)
    );
    assert.eq(0, ret);

    // make sure the data was restored correctly
    ['withOptions', 'withoutOptions', 'capped'].forEach(function(collName) {
        assert.eq(50, testDB[collName].count());
    });

    // make sure the options were not restored
    cappedOptionsFromDB = testDB['system.namespaces'].findOne({ name: 'test.capped' });
    assert.eq(undefined, cappedOptionsFromDB.options);
    withOptionsFromDB = testDB['system.namespaces'].findOne({ name: 'test.withOptions' });
    assert.eq(undefined, withOptionsFromDB.options);
    withoutOptionsFromDB = testDB['system.namespaces'].findOne({ name: 'test.withoutOptions' });
    assert.eq(undefined, withoutOptionsFromDB.options);

    // additional check that the capped collection is no longer capped
    var cappedStats = testDB.capped.stats();
    assert(!cappedStats.capped);

    // success
    toolTest.stop();

}());
