/**
 * Test the upgrade process for 3.2 ~~> the latest version involving indexes with invalid key
 * patterns:
 *   - Having an index with an invalid key pattern should cause the mongod to fail to start up.
 *   - A request to build an index with an invalid key pattern should cause a secondary running the
 *     latest version and syncing off a primary running 3.2 to fassert.
 */
(function() {
    'use strict';

    var testCases = [
        {a: 0},
        {a: NaN},
        {a: true},
    ];

    // The mongod should not start up when an index with an invalid key pattern exists.
    testCases.forEach(function(indexKeyPattern) {
        jsTest.log('Upgrading from a 3.2 instance to the latest version. This should fail when an' +
                   ' index with an invalid key pattern ' + tojson(indexKeyPattern) + ' exists');

        var dbpath = MongoRunner.dataPath + 'invalid_key_pattern_upgrade';
        resetDbpath(dbpath);

        var defaultOptions = {
            dbpath: dbpath,
            noCleanData: true,
        };

        // Start the old version.
        var oldVersionOptions = Object.extend({binVersion: '3.2'}, defaultOptions);
        var conn = MongoRunner.runMongod(oldVersionOptions);
        assert.neq(
            null, conn, 'mongod was unable to start up with options ' + tojson(oldVersionOptions));

        // Use write commands in order to make assertions about the success of operations based on
        // the response from the server.
        conn.forceWriteMode('commands');
        assert.commandWorked(conn.getDB('test').coll.createIndex(indexKeyPattern),
                             'failed to create index with key pattern' + tojson(indexKeyPattern));
        MongoRunner.stopMongod(conn);

        // Start the newest version.
        conn = MongoRunner.runMongod(defaultOptions);
        assert.eq(null,
                  conn,
                  'mongod should not have been able to start up when an index with' +
                      ' an invalid key pattern' + tojson(indexKeyPattern) + ' exists');
    });

    // Create a replica set with a primary running 3.2 and a secondary running the latest version.
    // The secondary should terminate when the command to build an index with an invalid key pattern
    // replicates.
    testCases.forEach(function(indexKeyPattern) {
        var replSetName = 'invalid_key_pattern_replset';
        var nodes = [
            {binVersion: '3.2'},
            {binVersion: 'latest'},
        ];

        var rst = new ReplSetTest({name: replSetName, nodes: nodes});

        var conns = rst.startSet({startClean: true});

        // Use write commands in order to make assertions about success of operations based on the
        // response.
        conns.forEach(function(conn) {
            conn.forceWriteMode('commands');
        });

        // Rig the election so that the 3.2 node becomes the primary.
        var replSetConfig = rst.getReplSetConfig();
        replSetConfig.members[1].priority = 0;

        rst.initiate(replSetConfig);

        var primary32 = conns[0];
        var secondaryLatest = conns[1];

        assert.commandWorked(primary32.getDB('test').coll.createIndex(indexKeyPattern),
                             'failed to create index with key pattern ' + tojson(indexKeyPattern));

        // Verify that the secondary running the latest version terminates when the command to build
        // an index with an invalid key pattern replicates.
        assert.soon(
            function() {
                try {
                    secondaryLatest.getDB('test').runCommand({ping: 1});
                } catch (e) {
                    return true;
                }
                return false;
            },
            'secondary should have terminated due to request to build an index with an invalid key' +
                ' pattern ' + tojson(indexKeyPattern));

        rst.stopSet(undefined, undefined, {allowedExitCodes: [MongoRunner.EXIT_ABRUPT]});
    });
})();
