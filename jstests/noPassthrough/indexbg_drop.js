/**
 * Initiates a background index build on the primary, and while the secondary is building the index
 * through replication, the primary drops all the indexes.
 * @tags: [requires_replication]
 */
(function() {
    'use strict';

    load('jstests/noPassthrough/libs/index_build.js');

    var dbname = 'dropbgindex';
    var collection = 'jstests_feh';
    var size = 100;

    // Setup the replica set.
    var replTest = new ReplSetTest({name: 'bgIndex', nodes: 3});
    var nodes = replTest.nodeList();
    printjson(nodes);

    // We need an arbiter to ensure that the primary doesn't step down when we restart the
    // secondary.
    replTest.startSet();
    replTest.initiate({
        "_id": "bgIndex",
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2], "arbiterOnly": true}
        ]
    });

    var master = replTest.getPrimary();
    var second = replTest.getSecondary();

    var masterDB = master.getDB(dbname);
    var secondDB = second.getDB(dbname);

    var dc = {dropIndexes: collection, index: "i_1"};

    // Setup collections.
    masterDB.dropDatabase();
    jsTest.log("Creating test data " + size + " documents");
    Random.setRandomSeed();
    var bulk = masterDB.getCollection(collection).initializeUnorderedBulkOp();
    for (i = 0; i < size; ++i) {
        bulk.insert({i: Random.rand()});
    }
    assert.writeOK(bulk.execute({w: 2, wtimeout: replTest.kDefaultTimeoutMS}));

    assert.commandWorked(secondDB.adminCommand(
        {configureFailPoint: "hangAfterStartingIndexBuild", mode: "alwaysOn"}));

    jsTest.log("Starting background indexing for test of: " + tojson(dc));

    // Add another index to be sure the drop command works.
    masterDB.getCollection(collection).ensureIndex({b: 1});
    masterDB.getCollection(collection).ensureIndex({i: 1}, {background: true});

    // Make sure the index build has started on the secondary.
    IndexBuildTest.waitForIndexBuildToStart(secondDB);

    jsTest.log("Dropping indexes");
    masterDB.runCommand({dropIndexes: collection, index: "*"});

    jsTest.log("Waiting on replication");
    assert.commandWorked(
        secondDB.adminCommand({configureFailPoint: "hangAfterStartingIndexBuild", mode: "off"}));
    replTest.awaitReplication();

    print("Index list on master:");
    masterDB.getCollection(collection).getIndexes().forEach(printjson);

    // Need to assert.soon because the drop only marks the index for removal
    // the removal itself is asynchronous and may take another moment before it happens.
    var i = 0;
    assert.soon(function() {
        print("Index list on secondary (run " + i + "):");
        secondDB.getCollection(collection).getIndexes().forEach(printjson);

        i++;
        return 1 === secondDB.getCollection(collection).getIndexes().length;
    }, "secondary did not drop index");

    replTest.stopSet();
}());
