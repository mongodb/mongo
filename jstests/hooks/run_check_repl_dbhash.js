// Runner for checkDBHashes() that runs the dbhash command on all replica set nodes
// to ensure all nodes have the same data.
'use strict';

(function() {
    load('jstests/libs/parallelTester.js');

    function isMasterSlave(uri) {
        const mongo = new Mongo(uri);
        jsTest.authenticate(mongo);
        const cmdLineOpts = mongo.getDB('admin').adminCommand('getCmdLineOpts');
        assert.commandWorked(cmdLineOpts);
        return cmdLineOpts.parsed.master === true;
    }

    function isMultiNodeReplSet(uri) {
        const mongo = new Mongo(uri);
        let hosts = [];
        const isMaster = mongo.adminCommand({isMaster: 1});
        if (isMaster.hasOwnProperty('setName')) {
            let hosts = isMaster.hosts;
            if (isMaster.hasOwnProperty('passives')) {
                hosts = hosts.concat(isMaster.passives);
            }
        }
        return hosts.length > 1;
    }

    // Adds the uri and description (replset or master-slave) if server needs dbhash check.
    function checkAndAddServerDesc(uri, out) {
        // No need to check the dbhash of single node replsets.
        if (isMultiNodeReplSet(uri)) {
            out.push({type: 'replset', uri: uri});
        } else if (isMasterSlave(uri)) {
            out.push({type: 'master-slave', uri: uri});
        }
    }

    function checkReplDataHashThread(serverDesc, testData, excludedDBs) {
        // A thin wrapper around master/slave nodes that provides the getHashes(), getPrimary(),
        // awaitReplication(), and nodeList() methods.
        // DEPRECATED: this wrapper only supports nodes started through resmoke's masterslave.py
        // fixture. Please do not use it with other master/slave clusters.
        function MasterSlaveDBHashTest(primaryHost) {
            const master = new Mongo(primaryHost);
            const masterPort = master.host.split(':')[1];
            const slave = new Mongo('localhost:' + String(parseInt(masterPort) + 1));

            this.nodeList = function() {
                return [master.host, slave.host];
            };

            this.getHashes = function(db) {
                const combinedRes = {};
                let res = master.getDB(db).runCommand('dbhash');
                assert.commandWorked(res);
                combinedRes.master = res;

                res = slave.getDB(db).runCommand('dbhash');
                assert.commandWorked(res);
                combinedRes.slaves = [res];

                return combinedRes;
            };

            this.getPrimary = function() {
                slave.setSlaveOk();
                this.liveNodes = {master: master, slaves: [slave]};
                return master;
            };

            this.getSecondaries = function() {
                return [slave];
            };

            this.awaitReplication = function() {
                assert.commandWorked(master.adminCommand({fsyncUnlock: 1}),
                                     'failed to unlock the primary');

                print('Starting fsync on master to flush all pending writes');
                assert.commandWorked(master.adminCommand({fsync: 1}));
                print('fsync on master completed');

                const kTimeout = 60 * 1000 * 5;  // 5min timeout
                const dbNames = master.getDBNames();
                print('Awaiting replication of inserts into ' + dbNames);
                for (let dbName of dbNames) {
                    if (dbName === 'local')
                        continue;
                    assert.writeOK(
                        master.getDB(dbName).await_repl.insert(
                            {awaiting: 'repl'}, {writeConcern: {w: 2, wtimeout: kTimeout}}),
                        'Awaiting replication failed');
                }
                print('Finished awaiting replication');
                assert.commandWorked(master.adminCommand({fsync: 1, lock: 1}),
                                     'failed to re-lock the primary');
            };

            this.checkReplicatedDataHashes = function() {
                ReplSetTest({nodes: 0}).checkReplicatedDataHashes.apply(this, ['test', [], true]);
            };

            this.checkReplicaSet = function() {
                ReplSetTest({nodes: 0}).checkReplicaSet.apply(this, arguments);
            };

            this.dumpOplog = function() {
                print('master-slave cannot dump oplog');
            };
        }

        TestData = testData;

        // Since UUIDs aren't explicitly replicated in master-slave deployments, we ignore the UUID
        // in the output of the 'listCollections' command to avoid reporting a known data
        // inconsistency issue from checkReplicatedDataHashes().
        const ignoreUUIDs = serverDesc.type === 'master-slave';
        let fixture = null;
        if (serverDesc.type === 'replset') {
            fixture = new ReplSetTest(serverDesc.uri);
        } else if (serverDesc.type === 'master-slave') {
            fixture = new MasterSlaveDBHashTest(serverDesc.uri);
        } else {
            throw 'unrecognized server type ' + serverDesc.type;
        }
        fixture.checkReplicatedDataHashes(undefined, excludedDBs, ignoreUUIDs);
    }

    let startTime = Date.now();
    assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a mongod?');

    // stores each server type (master/slave or replset) and uri.
    const serversNeedingReplDataHashCheck = [];
    const primaryInfo = db.isMaster();
    const isMongos = primaryInfo.msg === 'isdbgrid';
    const isReplSet = primaryInfo.hasOwnProperty('setName');
    const uri = db.getMongo().host;

    assert(primaryInfo.ismaster,
           'shell is not connected to the primary or master node: ' + tojson(primaryInfo));

    assert(isMongos || isReplSet || isMasterSlave(uri),
           'not replset, master/slave, or sharded cluster');

    if (isMongos) {
        // Add shards and config server if they are replica sets.
        let res = db.adminCommand('getShardMap');
        assert.commandWorked(res);
        const csURI = res.map.config;
        res = db.adminCommand('listShards');
        assert.commandWorked(res);
        const shardURIs = res.shards.map((shard) => shard.host);

        checkAndAddServerDesc(csURI, serversNeedingReplDataHashCheck);
        shardURIs.forEach((shardURI) => {
            checkAndAddServerDesc(shardURI, serversNeedingReplDataHashCheck);
        });
    } else {
        checkAndAddServerDesc(uri, serversNeedingReplDataHashCheck);
    }

    const threads = [];
    const excludedDBs = jsTest.options().excludedDBsFromDBHash || [];
    serversNeedingReplDataHashCheck.forEach((serverDesc) => {
        const thread = new ScopedThread(checkReplDataHashThread, serverDesc, TestData, excludedDBs);
        threads.push({serverDesc: serverDesc, handle: thread});
        thread.start();
    });

    if (serversNeedingReplDataHashCheck.length === 0) {
        let skipReason = 'No multi-node replication detected in ';
        if (isMongos) {
            skipReason += 'sharded cluster';
        } else if (isReplSet) {
            skipReason += 'replica set';
        } else {
            skipReason += 'master-slave set';
        }

        print('Skipping consistency checks for cluster because ' + skipReason);
        return;
    }

    const failedChecks = [];
    threads.forEach(thread => {
        thread.handle.join();
        if (thread.handle.hasFailed()) {
            failedChecks.push(thread.serverDesc.uri + ' (' + thread.serverDesc.type + ')');
        }
    });

    assert.eq(failedChecks.length,
              0,
              'dbhash check failed for the following hosts: ' + failedChecks.join(','));

    const totalTime = Date.now() - startTime;
    print('Finished consistency checks of cluster in ' + totalTime + ' ms.');
})();
