/**
 * Test the upgrade process for 3.0 ~~> the latest version involving partial indexes:
 *   - An invalid usage of partialFilterExpression should cause the mongod to fail to start up.
 *   - A request to build an invalid partial index should cause a secondary running the latest
 *     version and syncing off a primary running 3.0 to fassert.
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

    // Create a replica set with a primary running 3.0 and a secondary running the latest version.
    // The secondary should terminate when the command to build an invalid partial index replicates.
    testCases.forEach(function(indexOptions) {
        var replSetName = 'partial_index_replset';
        var nodes = [
            {binVersion: '3.0'},
            {binVersion: 'latest'},
        ];

        var rst = new ReplSetTest({name: replSetName, nodes: nodes});

        var conns = rst.startSet({startClean: true});

        // Use write commands in order to make assertions about success of operations based on the
        // response.
        conns.forEach(function(conn) {
            conn.forceWriteMode('commands');
        });

        // Rig the election so that the 3.0 node becomes the primary.
        var replSetConfig = rst.getReplSetConfig();
        replSetConfig.members[1].priority = 0;

        rst.initiate(replSetConfig);

        var primary30 = conns[0];
        var secondaryLatest = conns[1];

        assert.commandWorked(primary30.getDB('test').coll.createIndex({a: 1}, indexOptions),
                             'failed to create index with options ' + tojson(indexOptions));

        // Verify that the secondary running the latest version terminates when the command to build
        // an invalid partial index replicates.
        assert.soon(
            function() {
                try {
                    secondaryLatest.getDB('test').runCommand({ping: 1});
                } catch (e) {
                    return true;
                }
                return false;
            },
            'secondary should have terminated due to request to build an invalid partial index' +
                ' with options ' + tojson(indexOptions));

        rst.stopSet(undefined, undefined, {allowedExitCodes: [MongoRunner.EXIT_ABRUPT]});
    });
})();
