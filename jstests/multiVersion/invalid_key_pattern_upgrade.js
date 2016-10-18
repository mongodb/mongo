/**
 * Test the upgrade process for 3.2 ~~> the latest version involving indexes with invalid key
 * patterns:
 *   - Despite an invalid key pattern produced on a 3.2 mongod, the most recent version should be
 *   able to start up. The server should enforce old validation rules for existing v:0 and v:1
 *   indexes. On the other hand, all v:2 indexes, as well as any new index build request, should use
 *   the new and stricter validation rules.
 *   - If a 3.4 secondary is syncing off a 3.2 primary, it should be able to apply an index build
 *   operation containing an invalid key pattern, so long as the index version is less than v:2.
 */
(function() {
    'use strict';

    load('jstests/libs/get_index_helpers.js');

    // These key patterns are considered valid for existing v:0 and v:1 indexes, but are considered
    // invalid for v:2 indexes or new index builds.
    var testCases = [
        {a: 0},
        {a: NaN},
        {a: true},
    ];

    // The mongod should be able to start up when an index with an invalid key pattern exists.
    testCases.forEach(function(indexKeyPattern) {
        jsTest.log(
            'Upgrading from a 3.2 instance to the latest version. This should succeed when an' +
            ' index with an invalid key pattern ' + tojson(indexKeyPattern) + ' exists');

        var dbpath = MongoRunner.dataPath + 'invalid_key_pattern_upgrade';
        resetDbpath(dbpath);

        var defaultOptions = {
            dbpath: dbpath,
            noCleanData: true,
            storageEngine: jsTest.options().storageEngine
        };

        // Start the old version.
        var oldVersionOptions = Object.extend({binVersion: '3.2'}, defaultOptions);
        var conn = MongoRunner.runMongod(oldVersionOptions);
        assert.neq(
            null, conn, 'mongod was unable to start up with options ' + tojson(oldVersionOptions));

        // Use write commands in order to make assertions about the success of operations based on
        // the response from the server.
        conn.forceWriteMode('commands');
        assert.commandWorked(conn.getDB('test').coll.createIndex(indexKeyPattern, {name: 'badkp'}),
                             'failed to create index with key pattern' + tojson(indexKeyPattern));
        MongoRunner.stopMongod(conn);

        // Start the newest version.
        conn = MongoRunner.runMongod(defaultOptions);
        assert.neq(null,
                   conn,
                   'mongod was unable to start up when an index with' +
                       ' an invalid key pattern ' + tojson(indexKeyPattern) + ' exists');

        // Index created on 3.2 should be present in the catalog and should be v:1.
        let indexSpec = GetIndexHelpers.findByName(conn.getDB('test').coll.getIndexes(), 'badkp');
        assert.neq(null, indexSpec, 'could not find index "badkp"');
        assert.eq(1, indexSpec.v, tojson(indexSpec));

        // Collection compact command should succeed, despite the presence of the v:1 index which
        // would fail v:2 validation rules.
        assert.commandWorked(conn.getDB('test').runCommand({compact: 'coll'}));

        // repairDatabase should similarly succeed.
        assert.commandWorked(conn.getDB('test').runCommand({repairDatabase: 1}));

        // reIndex should succeed, since FCV is "3.2".
        assert.commandWorked(conn.getDB('test').coll.reIndex());

        // In FCV "3.4", compact and repairDatabase should succeed, but reIndex should fail since
        // the v:1 index cannot be upgraded to a valid v:2 index.
        assert.commandWorked(
            conn.getDB('test').adminCommand({setFeatureCompatibilityVersion: '3.4'}));
        assert.commandWorked(conn.getDB('test').runCommand({compact: 'coll'}));
        assert.commandWorked(conn.getDB('test').runCommand({repairDatabase: 1}));
        assert.commandFailed(conn.getDB('test').coll.reIndex());

        // Issuing the same index build against the newest version of the server should fail.
        assert.commandWorked(
            conn.getDB('test').adminCommand({setFeatureCompatibilityVersion: '3.2'}));
        assert.commandWorked(conn.getDB('test').coll.dropIndexes());
        assert.commandFailedWithCode(conn.getDB('test').coll.createIndex(indexKeyPattern),
                                     ErrorCodes.CannotCreateIndex,
                                     'creating index with key pattern ' + tojson(indexKeyPattern) +
                                         ' unexpectedly succeeded');

        // The index build should also fail when featureCompatibilityVersion is "3.4".
        assert.commandWorked(
            conn.getDB('test').adminCommand({setFeatureCompatibilityVersion: '3.4'}));
        assert.commandFailedWithCode(conn.getDB('test').coll.createIndex(indexKeyPattern),
                                     ErrorCodes.CannotCreateIndex,
                                     'creating index with key pattern ' + tojson(indexKeyPattern) +
                                         ' unexpectedly succeeded');

        // Index build should also fail if v:1 or v:2 is explicitly requested.
        assert.commandFailedWithCode(conn.getDB('test').coll.createIndex(indexKeyPattern, {v: 1}),
                                     ErrorCodes.CannotCreateIndex,
                                     'creating index with key pattern ' + tojson(indexKeyPattern) +
                                         ' unexpectedly succeeded');
        assert.commandFailedWithCode(conn.getDB('test').coll.createIndex(indexKeyPattern, {v: 2}),
                                     ErrorCodes.CannotCreateIndex,
                                     'creating index with key pattern ' + tojson(indexKeyPattern) +
                                         ' unexpectedly succeeded');

        MongoRunner.stopMongod(conn);
    });

    // Create a replica set with a primary running 3.2 and a secondary running the latest version.
    // The secondary should be able to build a v:1 index which would fail v:2 key pattern validation
    // rules.
    testCases.forEach(function(indexKeyPattern) {
        jsTest.log(
            'Syncing an index build from a 3.2 primary to a secondary on the latest version. ' +
            'This should succeed for key pattern: ' + tojson(indexKeyPattern));

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

        assert.commandWorked(
            primary32.getDB('test').coll.createIndex(indexKeyPattern, {name: 'badkp'}),
            'failed to create index with key pattern ' + tojson(indexKeyPattern));
        rst.awaitReplication();

        // Verify that the secondary has the v:1 index.
        let indexSpec =
            GetIndexHelpers.findByName(secondaryLatest.getDB('test').coll.getIndexes(), 'badkp');
        assert.neq(null, indexSpec, 'could not find index "badkp"');
        assert.eq(1, indexSpec.v, tojson(indexSpec));

        rst.stopSet();
    });
})();
