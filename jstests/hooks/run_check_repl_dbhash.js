// Runner for checkDBHashes() that runs the dbhash command on all replica set nodes
// to ensure all nodes have the same data.
'use strict';

(function() {
    load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.
    load('jstests/libs/parallelTester.js');     // For ScopedThread.

    // A thin wrapper around master/slave nodes that provides methods necessary for checking data
    // consistency between the master and slave nodes.
    //
    // DEPRECATED: This wrapper is only intended to be used for the master-slave deployment started
    // by resmoke.py as part of the master_slave_jscore_passthrough.yml test suite and it shouldn't
    // be used for any other master/slave tests.
    function MasterSlaveDBHashTest(primaryHost) {
        const master = new Mongo(primaryHost);
        const masterPort = master.host.split(':')[1];
        const slave = new Mongo('localhost:' + String(parseInt(masterPort) + 1));

        this.nodeList = function nodeList() {
            return [master.host, slave.host];
        };

        this.getHashes = function getHashes(db) {
            const combinedRes = {};
            let res = master.getDB(db).runCommand('dbhash');
            assert.commandWorked(res);
            combinedRes.master = res;

            res = slave.getDB(db).runCommand('dbhash');
            assert.commandWorked(res);
            combinedRes.slaves = [res];

            return combinedRes;
        };

        this.getPrimary = function getPrimary() {
            slave.setSlaveOk();
            this.liveNodes = {master: master, slaves: [slave]};
            return master;
        };

        this.getSecondaries = function getSecondaries() {
            return [slave];
        };

        this.awaitReplication = function awaitReplication() {
            assert.commandWorked(master.adminCommand({fsyncUnlock: 1}),
                                 'failed to unlock the primary');

            print('Starting fsync on master to flush all pending writes');
            assert.commandWorked(master.adminCommand({fsync: 1}));
            print('fsync on master completed');

            const kTimeout = 5 * 60 * 1000;  // 5 minute timeout
            const dbNames = master.getDBNames();

            for (let dbName of dbNames) {
                if (dbName === 'local') {
                    continue;
                }

                print('Awaiting replication of inserts into ' + dbName);
                assert.writeOK(master.getDB(dbName).await_repl.insert(
                                   {awaiting: 'repl'}, {writeConcern: {w: 2, wtimeout: kTimeout}}),
                               'Awaiting replication failed');
            }
            print('Finished awaiting replication');
            assert.commandWorked(master.adminCommand({fsync: 1, lock: 1}),
                                 'failed to re-lock the primary');
        };

        this.checkReplicatedDataHashes = function checkReplicatedDataHashes() {
            const msgPrefix = 'checkReplicatedDataHashes for master-slave deployment';
            const excludedDBs = jsTest.options().excludedDBsFromDBHash || [];

            // Since UUIDs aren't explicitly replicated in master-slave deployments, we ignore the
            // UUID in the output of the 'listCollections' command to avoid reporting a known data
            // inconsistency issue from checkReplicatedDataHashes().
            const ignoreUUIDs = true;

            new ReplSetTest({
                nodes: 0
            }).checkReplicatedDataHashes.call(this, msgPrefix, excludedDBs, ignoreUUIDs);
        };

        this.checkReplicaSet = function checkReplicaSet() {
            new ReplSetTest({nodes: 0}).checkReplicaSet.apply(this, arguments);
        };

        this.dumpOplog = function dumpOplog() {
            print('Not dumping oplog for master-slave deployment');
        };
    }

    function isMasterSlaveDeployment(conn) {
        const cmdLineOpts = assert.commandWorked(conn.adminCommand({getCmdLineOpts: 1}));
        return cmdLineOpts.parsed.master === true;
    }

    function checkReplicatedDataHashesThread(hosts, testData) {
        try {
            TestData = testData;
            new ReplSetTest(hosts[0]).checkReplicatedDataHashes();
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

        if (isMasterSlaveDeployment(conn)) {
            new MasterSlaveDBHashTest(conn.host).checkReplicatedDataHashes();
            return;
        }

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

            new ReplSetTest(topology.nodes[0]).checkReplicatedDataHashes();
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
