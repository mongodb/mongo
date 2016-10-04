/**
 * Test the upgrade process for 3.0 ~~> the latest version involving partial indexes:
 *   - An invalid usage of partialFilterExpression should cause the mongod to fail to start up.
 */
(function() {
    'use strict';

    var testCases = [
        {
          partialFilterExpression: 'not an object',
        },
        {
          partialFilterExpression: {field: {$regex: 'not a supported operator'}},
        },
        {
          partialFilterExpression: {field: 'cannot be combined with sparse=true'},
          sparse: true,
        },
    ];

    // The mongod should not start up when an invalid usage of partialFilterExpression exists.
    testCases.forEach(function(indexOptions) {
        jsTest.log('Upgrading from a 3.0 instance to the latest version. This should fail when an' +
                   ' index with options ' + tojson(indexOptions) + ' exists');

        var dbpath = MongoRunner.dataPath + 'partial_index_upgrade';
        resetDbpath(dbpath);

        var defaultOptions = {
            dbpath: dbpath,
            noCleanData: true,
            storageEngine: jsTest.options().storageEngine
        };

        // Start the old version.
        var oldVersionOptions = Object.extend({binVersion: '3.0'}, defaultOptions);
        var conn = MongoRunner.runMongod(oldVersionOptions);
        assert.neq(
            null, conn, 'mongod was unable to start up with options ' + tojson(oldVersionOptions));

        // Use write commands in order to make assertions about the success of operations based on
        // the response from the server.
        conn.forceWriteMode('commands');
        assert.commandWorked(conn.getDB('test').coll.createIndex({a: 1}, indexOptions),
                             'failed to create index with options ' + tojson(indexOptions));
        MongoRunner.stopMongod(conn);

        // Start the newest version.
        conn = MongoRunner.runMongod(defaultOptions);
        assert.eq(null,
                  conn,
                  'mongod should not have been able to start up when an index with' +
                      ' options ' + tojson(indexOptions) + ' exists');
    });
})();
