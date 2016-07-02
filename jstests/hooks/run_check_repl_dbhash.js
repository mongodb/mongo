// Runner for checkDBHashes() that runs the dbhash command on all replica set nodes
// to ensure all nodes have the same data.
'use strict';

(function() {
    // A thin wrapper around master/slave nodes that provides the getHashes(), getPrimary(),
    // awaitReplication(), and nodeList() methods.
    // DEPRECATED: this wrapper only supports nodes started through resmoke's masterslave.py
    // fixture. Please do not use it with other master/slave clusters.
    var MasterSlaveDBHashTest = function(primaryHost) {
        var master = new Mongo(primaryHost);
        var resolvedHost = getHostName();
        var masterPort = master.host.split(':')[1];
        // The 'host' property is modified manually because 'localhost' is used by default in a new
        // Mongo() connection. We set the value to the real hostname because that is what the server
        // uses.
        master.host = resolvedHost + ':' + masterPort;

        var slave = new Mongo(resolvedHost + ':' + String(parseInt(masterPort) + 1));

        this.nodeList = function() {
            return [master.host, slave.host];
        };

        this.getHashes = function(db) {
            var combinedRes = {};
            var res = master.getDB(db).runCommand("dbhash");
            assert.commandWorked(res);
            combinedRes.master = res;

            res = slave.getDB(db).runCommand("dbhash");
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
            assert.commandWorked(primary.adminCommand({fsyncUnlock: 1}),
                                 'failed to unlock the primary');

            print('Starting fsync on master to flush all pending writes');
            assert.commandWorked(master.adminCommand({fsync: 1}));
            print('fsync on master completed');

            var timeout = 60 * 1000 * 5;  // 5min timeout
            var dbNames = master.getDBNames();
            print('Awaiting replication of inserts into ' + dbNames);
            for (var dbName of dbNames) {
                if (dbName === 'local')
                    continue;
                assert.writeOK(master.getDB(dbName).await_repl.insert(
                                   {awaiting: 'repl'}, {writeConcern: {w: 2, wtimeout: timeout}}),
                               'Awaiting replication failed');
            }
            print('Finished awaiting replication');
            assert.commandWorked(primary.adminCommand({fsync: 1, lock: 1}),
                                 'failed to re-lock the primary');
        };
    };

    var startTime = Date.now();
    assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a mongod?');

    var primaryInfo = db.isMaster();

    assert(primaryInfo.ismaster,
           'shell is not connected to the primary or master node: ' + tojson(primaryInfo));

    var rst;
    var cmdLineOpts = db.adminCommand('getCmdLineOpts');
    assert.commandWorked(cmdLineOpts);
    var isMasterSlave = cmdLineOpts.parsed.master === true;
    if (isMasterSlave) {
        rst = new MasterSlaveDBHashTest(db.getMongo().host);
    } else {
        rst = new ReplSetTest(db.getMongo().host);
    }

    // Call getPrimary to populate rst with information about the nodes.
    var primary = rst.getPrimary();
    assert(primary, 'calling getPrimary() failed');

    var activeException = false;

    try {
        // Lock the primary to prevent the TTL monitor from deleting expired documents in
        // the background while we are getting the dbhashes of the replica set members.
        assert.commandWorked(primary.adminCommand({fsync: 1, lock: 1}),
                             'failed to lock the primary');
        rst.awaitReplication(60 * 1000 * 5);  // 5min timeout

        var phaseName = 'after test hook';
        load('jstests/hooks/check_repl_dbhash.js');
        var blacklist = [];
        checkDBHashes(rst, blacklist, phaseName);
    } catch (e) {
        activeException = true;
        throw e;
    } finally {
        // Allow writes on the primary.
        var res = primary.adminCommand({fsyncUnlock: 1});

        if (!res.ok) {
            var msg = 'failed to unlock the primary, which may cause this' +
                ' test to hang: ' + tojson(res);
            if (activeException) {
                print(msg);
            } else {
                throw new Error(msg);
            }
        }
    }

    var totalTime = Date.now() - startTime;
    print('Finished consistency checks of cluster in ' + totalTime + ' ms.');
})();
