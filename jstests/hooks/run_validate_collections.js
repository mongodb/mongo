// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
    load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.
    load('jstests/libs/parallelTester.js');     // For ScopedThread.

    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
    const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

    const hostList = [];

    if (topology.type === Topology.kStandalone) {
        hostList.push(topology.mongod);
    } else if (topology.type === Topology.kReplicaSet) {
        hostList.push(...topology.nodes);
    } else if (topology.type === Topology.kShardedCluster) {
        hostList.push(...topology.configsvr.nodes);

        for (let shardName of Object.keys(topology.shards)) {
            const shard = topology.shards[shardName];

            if (shard.type === Topology.kStandalone) {
                hostList.push(shard.mongod);
            } else if (shard.type === Topology.kReplicaSet) {
                hostList.push(...shard.nodes);
            } else {
                throw new Error('Unrecognized topology format: ' + tojson(topology));
            }
        }
    } else {
        throw new Error('Unrecognized topology format: ' + tojson(topology));
    }

    // Run a separate thread to validate collections on each server in parallel.
    var validateCollectionsThread = function(host, testData) {
        load('jstests/hooks/validate_collections.js');  // For validateCollections.
        TestData = testData;  // Pass the TestData object from main thread.

        try {
            print('Running validate() on ' + host);
            const conn = new Mongo(host);
            conn.setSlaveOk();
            jsTest.authenticate(conn);

            const dbNames = conn.getDBNames();
            for (let dbName of dbNames) {
                if (!validateCollections(conn.getDB(dbName), {full: true})) {
                    return {ok: 0};
                }
            }
            return {ok: 1};
        } catch (e) {
            print('Exception caught in scoped thread running validationCollections on server: ' +
                  host);
            return {ok: 0, error: e.toString(), stack: e.stack};
        }
    };

    // We run the scoped threads in a try/finally block in case any thread throws an exception, in
    // which case we want to still join all the threads.
    let threads = [];

    try {
        hostList.forEach(host => {
            const thread = new ScopedThread(validateCollectionsThread, host, TestData);
            threads.push(thread);
            thread.start();
        });
    } finally {
        // Wait for each thread to finish. Throw an error if any thread fails.
        const returnData = threads.map(thread => {
            thread.join();
            return thread.returnData();
        });

        returnData.forEach(res => {
            assert.commandWorked(res, 'Collection validation failed');
        });
    }
})();
