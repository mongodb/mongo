/**
 * Tests that different values for the same configuration string key have the following order of
 * preference:
 *   1. index-specific options specified to createIndex().
 *   2. collection-wide options specified as "indexOptionDefaults" to createCollection().
 *   3. system-wide options specified by --wiredTigerIndexConfigString or by
 *     inMemoryIndexConfigString.
 */
(function() {
    'use strict';

    var engine = 'wiredTiger';
    if (jsTest.options().storageEngine) {
        engine = jsTest.options().storageEngine;
    }

    // Skip this test if not running with the right storage engine.
    if (engine !== 'wiredTiger' && engine !== 'inMemory') {
        jsTest.log('Skipping test because storageEngine is not "wiredTiger" or "inMemory"');
        return;
    }

    // Skip this test when 'xxxIndexConfigString' is already set in TestData.
    // TODO: This test can be enabled when MongoRunner supports combining WT config strings with
    // commas.
    if (jsTest.options()[engine + 'IndexConfigString']) {
        jsTest.log('Skipping test because system-wide defaults for index options are already set');
        return;
    }

    // Use different values for the same configuration string key to test that index-specific
    // options override collection-wide options, and that collection-wide options override
    // system-wide options.
    var systemWideConfigString = 'split_pct=70,';
    var collectionWideConfigString = 'split_pct=75,';
    var indexSpecificConfigString = 'split_pct=80,';

    // Start up a mongod with system-wide defaults for index options and create a collection without
    // any additional options. Tests than an index without any additional options should take on the
    // system-wide defaults, whereas an index with additional options should override the
    // system-wide defaults.
    runTest({});

    // Start up a mongod with system-wide defaults for index options and create a collection with
    // additional options. Tests than an index without any additional options should take on the
    // collection-wide defaults, whereas an index with additional options should override the
    // collection-wide defaults.
    runTest({indexOptionDefaults: collectionWideConfigString});

    function runTest(collOptions) {
        var hasIndexOptionDefaults = collOptions.hasOwnProperty('indexOptionDefaults');

        var dbpath = MongoRunner.dataPath + 'wt_index_option_defaults';
        resetDbpath(dbpath);

        // Start a mongod with system-wide defaults for engine-specific index options.
        var conn = MongoRunner.runMongod({
            dbpath: dbpath,
            noCleanData: true,
            [engine + 'IndexConfigString']: systemWideConfigString,
        });
        assert.neq(null, conn, 'mongod was unable to start up');

        var testDB = conn.getDB('test');
        var cmdObj = {create: 'coll'};

        // Apply collection-wide defaults for engine-specific index options if any were
        // specified.
        if (hasIndexOptionDefaults) {
            cmdObj.indexOptionDefaults = {
                storageEngine: {[engine]: {configString: collOptions.indexOptionDefaults}}
            };
        }
        assert.commandWorked(testDB.runCommand(cmdObj));

        // Create an index that does not specify any engine-specific options.
        assert.commandWorked(testDB.coll.createIndex({a: 1}, {name: 'without_options'}));

        // Create an index that specifies engine-specific index options.
        assert.commandWorked(testDB.coll.createIndex({b: 1}, {
            name: 'with_options',
            storageEngine: {[engine]: {configString: indexSpecificConfigString}}
        }));

        var collStats = testDB.runCommand({collStats: 'coll'});
        assert.commandWorked(collStats);

        checkIndexWithoutOptions(collStats.indexDetails);
        checkIndexWithOptions(collStats.indexDetails);

        MongoRunner.stopMongod(conn);

        function checkIndexWithoutOptions(indexDetails) {
            var indexSpec = getIndexSpecByName(testDB.coll, 'without_options');
            assert(!indexSpec.hasOwnProperty('storageEngine'),
                   'no storage engine options should have been set in the index spec: ' +
                       tojson(indexSpec));

            var creationString = indexDetails.without_options.creationString;
            if (hasIndexOptionDefaults) {
                assert.eq(-1,
                          creationString.indexOf(systemWideConfigString),
                          'system-wide index option present in the creation string even though a ' +
                              'collection-wide option was specified: ' + creationString);
                assert.lte(0,
                           creationString.indexOf(collectionWideConfigString),
                           'collection-wide index option not present in the creation string: ' +
                               creationString);
            } else {
                assert.lte(0,
                           creationString.indexOf(systemWideConfigString),
                           'system-wide index option not present in the creation string: ' +
                               creationString);
                assert.eq(-1,
                          creationString.indexOf(collectionWideConfigString),
                          'collection-wide index option present in creation string even though ' +
                              'it was not specified: ' + creationString);
            }

            assert.eq(-1,
                      creationString.indexOf(indexSpecificConfigString),
                      'index-specific option present in creation string even though it was not' +
                          ' specified: ' + creationString);
        }

        function checkIndexWithOptions(indexDetails) {
            var indexSpec = getIndexSpecByName(testDB.coll, 'with_options');
            assert(indexSpec.hasOwnProperty('storageEngine'),
                   'storage engine options should have been set in the index spec: ' +
                       tojson(indexSpec));
            assert.docEq({[engine]: {configString: indexSpecificConfigString}},
                         indexSpec.storageEngine,
                         engine + ' index options not present in the index spec');

            var creationString = indexDetails.with_options.creationString;
            assert.eq(-1,
                      creationString.indexOf(systemWideConfigString),
                      'system-wide index option present in the creation string even though an ' +
                          'index-specific option was specified: ' + creationString);
            assert.eq(-1,
                      creationString.indexOf(collectionWideConfigString),
                      'system-wide index option present in the creation string even though an ' +
                          'index-specific option was specified: ' + creationString);
            assert.lte(
                0,
                creationString.indexOf(indexSpecificConfigString),
                'index-specific option not present in the creation string: ' + creationString);
        }
    }

    function getIndexSpecByName(coll, indexName) {
        var indexes = coll.getIndexes().filter(function(spec) {
            return spec.name === indexName;
        });
        assert.eq(1, indexes.length, 'index "' + indexName + '" not found');
        return indexes[0];
    }
})();
