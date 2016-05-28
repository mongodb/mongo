/**
 * Creates a replica set with a 3.2 primary and a 3.0 secondary. Tests that the
 * "indexOptionDefaults" specified to collection creation are replicated by the 3.2 primary, but
 * ignored by the 3.0 secondary.
 */
(function() {
    'use strict';

    var replSetName = 'index_option_defaults';
    var nodes = [
        {binVersion: 'latest', storageEngine: 'wiredTiger'},
        {binVersion: '3.0', storageEngine: 'wiredTiger'},
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
    replSetConfig.protocolVersion = 0;

    rst.initiate(replSetConfig);

    var primary32 = conns[0].getDB('test');
    var secondary30 = conns[1].getDB('test');

    // Create a collection with "indexOptionDefaults" specified.
    var indexOptions = {storageEngine: {wiredTiger: {configString: 'prefix_compression=false'}}};
    assert.commandWorked(primary32.runCommand({create: 'coll', indexOptionDefaults: indexOptions}));

    // Verify that the "indexOptionDefaults" field is present in the corresponding oplog entry.
    var entry =
        primary32.getSiblingDB('local').oplog.rs.find().sort({$natural: -1}).limit(1).next();
    assert.docEq(indexOptions,
                 entry.o.indexOptionDefaults,
                 'indexOptionDefaults were not replicated: ' + tojson(entry));

    rst.awaitReplication();

    var collectionInfos = secondary30.getCollectionInfos({name: 'coll'});
    assert.eq(1,
              collectionInfos.length,
              'collection "coll" was not created on the secondary: ' + tojson(collectionInfos));

    assert(!collectionInfos[0].options.hasOwnProperty('indexOptionDefaults'),
           'indexOptionDefaults should not have been applied: ' + tojson(collectionInfos));

    rst.stopSet();
})();

/**
 * Creates a replica set with a 3.0 primary and a 3.2 secondary. Tests that the
 * "indexOptionDefaults" specified on a collection creation are ignored by the 3.0 primary, but
 * still replicated to the 3.2 secondary.
 */
(function() {
    'use strict';

    var replSetName = 'default_index_options';
    var nodes = [
        {binVersion: '3.0', storageEngine: 'wiredTiger'},
        {binVersion: 'latest', storageEngine: 'wiredTiger'},
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

    var primary30 = conns[0].getDB('test');
    var secondary32 = conns[1].getDB('test');

    // Create a collection with "indexOptionDefaults" specified.
    var indexOptions = {storageEngine: {wiredTiger: {configString: 'prefix_compression=false'}}};
    assert.commandWorked(primary30.runCommand({create: 'coll', indexOptionDefaults: indexOptions}));

    // Verify that the "indexOptionDefaults" field is present in the corresponding oplog entry.
    var entry =
        primary30.getSiblingDB('local').oplog.rs.find().sort({$natural: -1}).limit(1).next();
    assert.docEq(indexOptions,
                 entry.o.indexOptionDefaults,
                 'indexOptionDefaults were not replicated: ' + tojson(entry));

    rst.awaitReplication();

    var collectionInfos = secondary32.getCollectionInfos({name: 'coll'});
    assert.eq(1,
              collectionInfos.length,
              'collection "coll" was not created on the secondary: ' + tojson(collectionInfos));

    assert.docEq(indexOptions,
                 collectionInfos[0].options.indexOptionDefaults,
                 'indexOptionDefaults were not applied: ' + tojson(collectionInfos));

    rst.stopSet();
})();
