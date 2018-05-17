// Runner for checkDBHashes() that runs the dbhash command on all replica set nodes
// to ensure all nodes have the same data.
'use strict';

(function() {
    load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.
    load('jstests/libs/parallelTester.js');     // For ScopedThread.

    function checkReplicatedDataHashesThread(hosts, testData) {
        try {
            TestData = testData;
            const excludedDBs = jsTest.options().excludedDBsFromDBHash;
            const rst = new ReplSetTest(hosts[0]);
            rst.checkReplicatedDataHashes(undefined, excludedDBs);
            if (TestData.checkCollectionCounts) {
                rst.checkCollectionCounts();
            }
            return {ok: 1};
        } catch (e) {
            return {ok: 0, hosts: hosts, error: e.toString(), stack: e.stack};
        }
    }

    const startTime = Date.now();
    assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a mongod?');

    let skipped = false;
    try {
        const conn = db.getMongo();
        const topology = DiscoverTopology.findConnectedNodes(conn);

        if (topology.type === Topology.kStandalone) {
            print('Skipping data consistency checks for cluster because we are connected to a' +
                  ' stand-alone mongod: ' + tojsononeline(topology));
            skipped = true;
            return;
        }

        if (topology.type === Topology.kReplicaSet) {
            if (topology.nodes.length === 1) {
                print('Skipping data consistency checks for cluster because we are connected to a' +
                      ' 1-node replica set: ' + tojsononeline(topology));
                skipped = true;
                return;
            }

            const excludedDBs = jsTest.options().excludedDBsFromDBHash;
            new ReplSetTest(topology.nodes[0]).checkReplicatedDataHashes(undefined, excludedDBs);
            return;
        }

        if (topology.type !== Topology.kShardedCluster) {
            throw new Error('Unrecognized topology format: ' + tojson(topology));
        }

        const threads = [];
        try {
            if (topology.configsvr.nodes.length > 1) {
                const thread = new ScopedThread(
                    checkReplicatedDataHashesThread, topology.configsvr.nodes, TestData);
                threads.push(thread);
                thread.start();
            } else {
                print('Skipping data consistency checks for 1-node CSRS: ' +
                      tojsononeline(topology));
            }

            for (let shardName of Object.keys(topology.shards)) {
                const shard = topology.shards[shardName];

                if (shard.type === Topology.kStandalone) {
                    print('Skipping data consistency checks for stand-alone shard: ' +
                          tojsononeline(topology));
                    continue;
                }

                if (shard.type !== Topology.kReplicaSet) {
                    throw new Error('Unrecognized topology format: ' + tojson(topology));
                }

                if (shard.nodes.length > 1) {
                    const thread =
                        new ScopedThread(checkReplicatedDataHashesThread, shard.nodes, TestData);
                    threads.push(thread);
                    thread.start();
                } else {
                    print('Skipping data consistency checks for 1-node replica set shard: ' +
                          tojsononeline(topology));
                }
            }
        } finally {
            // Wait for each thread to finish. Throw an error if any thread fails.
            const returnData = threads.map(thread => {
                thread.join();
                return thread.returnData();
            });

            returnData.forEach(res => {
                assert.commandWorked(res, 'data consistency checks failed');
            });
        }
    } finally {
        if (!skipped) {
            const totalTime = Date.now() - startTime;
            print('Finished data consistency checks for cluster in ' + totalTime + ' ms.');
        }
    }
})();
