/**
 * Test the upgrade process for 2.6 ~~> 3.2 and 3.0 ~~> 3.2, where mmapv1 should continue to be the
 * default storage engine. Repeat the process with --directoryperdb set.
 */
(function() {
    'use strict';

    var testCases = [
        {
          binVersion: '2.6',
        },
        {
          binVersion: '2.6',
          directoryperdb: '',
        },
        {
          binVersion: '3.0',
        },
        {
          binVersion: '3.0',
          directoryperdb: '',
        },
    ];

    // The mongod should start up with mmapv1 when the --storageEngine flag is omitted, or when
    // --storageEngine=mmapv1 is explicitly specified.
    testCases.forEach(function(testCase) {
        [null, 'mmapv1'].forEach(function(storageEngine) {
            jsTest.log('Upgrading from a ' + testCase.binVersion + ' instance with options=' +
                       tojson(testCase) + ' to the latest version. This should succeed when the' +
                       ' latest version ' +
                       (storageEngine ? ('explicitly specifies --storageEngine=' + storageEngine)
                                      : 'omits the --storageEngine flag'));

            var dbpath = MongoRunner.dataPath + 'mmapv1_overrides_default_storage_engine';
            resetDbpath(dbpath);

            var defaultOptions = {
                dbpath: dbpath,
                noCleanData: true,
            };

            // Start the old version.
            var mongodOptions = Object.merge(defaultOptions, testCase);
            var conn = MongoRunner.runMongod(mongodOptions);
            assert.neq(
                null, conn, 'mongod was unable to start up with options ' + tojson(mongodOptions));
            assert.commandWorked(conn.getDB('test').runCommand({ping: 1}));
            MongoRunner.stopMongod(conn);

            // Start the newest version.
            mongodOptions = Object.extend({}, defaultOptions);
            if (storageEngine) {
                mongodOptions.storageEngine = storageEngine;
            }
            if (testCase.hasOwnProperty('directoryperdb')) {
                mongodOptions.directoryperdb = testCase.directoryperdb;
            }
            conn = MongoRunner.runMongod(mongodOptions);
            assert.neq(
                null, conn, 'mongod was unable to start up with options ' + tojson(mongodOptions));
            assert.commandWorked(conn.getDB('test').runCommand({ping: 1}));
            MongoRunner.stopMongod(conn);
        });
    });

    // The mongod should not start up when --storageEngine=wiredTiger is specified.
    testCases.forEach(function(testCase) {
        jsTest.log('Upgrading from a ' + testCase.binVersion + ' instance with options=' +
                   tojson(testCase) + ' to the latest version. This should fail when the latest' +
                   ' version specifies --storageEngine=wiredTiger');

        var dbpath = MongoRunner.dataPath + 'mmapv1_overrides_default_storage_engine';
        resetDbpath(dbpath);

        var defaultOptions = {
            dbpath: dbpath,
            noCleanData: true,
        };

        // Start the old version.
        var mongodOptions = Object.merge(defaultOptions, testCase);
        var conn = MongoRunner.runMongod(mongodOptions);
        assert.neq(
            null, conn, 'mongod was unable to start up with options ' + tojson(mongodOptions));
        assert.commandWorked(conn.getDB('test').runCommand({ping: 1}));
        MongoRunner.stopMongod(conn);

        // Start the newest version.
        mongodOptions = Object.extend({storageEngine: 'wiredTiger'}, defaultOptions);
        conn = MongoRunner.runMongod(mongodOptions);
        assert.eq(
            null,
            conn,
            'mongod should not have been able to start up with options ' + tojson(mongodOptions));
    });
}());
