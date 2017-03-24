/**
 * Sets up a test for the backup/restore process:
 * - 3 node replica set
 * - Mongo CRUD client
 * - Mongo FSM client
 * - fsyncLock (or stop) Secondary
 * - cp (or rsync) DB files
 * - fsyncUnlock (or start) Secondary
 * - Start mongod as hidden secondary
 * - Wait until new hidden node becomes secondary
 *
 * @param {Object} options An object with the following fields:
 *   {
 *     backup {string}: backup method. Must be one of: fsyncLock, rolling, stopStart. Required.
 *     nodes {number}: number of nodes in replica set initially (excluding hidden secondary node to
 *                     be added during test). Default: 3.
 *     clientTime {number}: Time (in milliseconds) for clients to run before getting the new
 *                          secondary its data. Default: 10,000.
 *   }
 */

var BackupRestoreTest = function(options) {
    "use strict";

    if (!(this instanceof BackupRestoreTest)) {
        return new BackupRestoreTest(options);
    }

    // Capture the 'this' reference
    var self = this;

    self.options = options;

    /**
     * Runs a command in the bash shell.
     */
    function _runCmd(cmd) {
        runProgram('bash', '-c', cmd);
    }

    /**
     * Starts a client that will run a CRUD workload.
     */
    function _crudClient(host, dbName, collectionName) {
        // Launch CRUD client
        var crudClientCmds = function(dbName, collectionName) {
            var bulkNum = 1000;
            var baseNum = 100000;
            var coll = db.getSiblingDB(dbName).getCollection(collectionName);
            coll.ensureIndex({x: 1});
            var largeValue = new Array(1024).join('L');
            Random.setRandomSeed();
            // Run indefinitely.
            while (true) {
                try {
                    var op = Random.rand();
                    var match = Math.floor(Random.rand() * baseNum);
                    if (op < 0.2) {
                        // 20% of the operations: bulk insert bulkNum docs.
                        var bulk = coll.initializeUnorderedBulkOp();
                        for (var i = 0; i < bulkNum; i++) {
                            bulk.insert({
                                x: (match * i) % baseNum,
                                doc: largeValue.substring(0, match % largeValue.length),
                            });
                        }
                        assert.writeOK(bulk.execute());
                    } else if (op < 0.4) {
                        // 20% of the operations: update docs.
                        var updateOpts = {upsert: true, multi: true};
                        assert.writeOK(coll.update({x: {$gte: match}},
                                                   {$inc: {x: baseNum}, $set: {n: 'hello'}},
                                                   updateOpts));
                    } else if (op < 0.9) {
                        // 50% of the operations: find matchings docs.
                        // itcount() consumes the cursor
                        coll.find({x: {$gte: match}}).itcount();
                    } else {
                        // 10% of the operations: remove matching docs.
                        assert.writeOK(coll.remove({x: {$gte: match}}));
                    }
                } catch (e) {
                    if (e instanceof ReferenceError || e instanceof TypeError) {
                        throw e;
                    }
                }
            }
        };

        // Returns the pid of the started mongo shell so the CRUD test client can be terminated
        // without waiting for its execution to finish.
        return startMongoProgramNoConnect(
            'mongo',
            '--eval',
            '(' + crudClientCmds + ')("' + dbName + '", "' + collectionName + '")',
            host);
    }

    /**
     * Starts a client that will run a FSM workload.
     */
    function _fsmClient(host, blackListDb, numNodes) {
        // Launch FSM client
        // SERVER-19488 The FSM framework assumes that there is an implicit 'db' connection when
        // started without any cluster options. Since the shell running this test was started with
        // --nodb, another mongo shell is used to allow implicit connections to be made to the
        // primary of the replica set.
        var fsmClientCmds = function(blackListDb, numNodes) {
            'use strict';
            load('jstests/concurrency/fsm_libs/runner.js');
            var dir = 'jstests/concurrency/fsm_workloads';
            var blacklist = [
                // Disabled due to MongoDB restrictions and/or workload restrictions
                'agg_group_external.js',  // uses >100MB of data, which can overwhelm test hosts
                'agg_sort_external.js',   // uses >100MB of data, which can overwhelm test hosts
                'auth_create_role.js',
                'auth_create_user.js',
                'auth_drop_role.js',
                'auth_drop_user.js',
                'create_index_background.js',
                'findAndModify_update_grow.js',  // can cause OOM kills on test hosts
                'reindex_background.js',
                'rename_capped_collection_chain.js',
                'rename_capped_collection_dbname_chain.js',
                'rename_capped_collection_dbname_droptarget.js',
                'rename_capped_collection_droptarget.js',
                'rename_collection_chain.js',
                'rename_collection_dbname_chain.js',
                'rename_collection_dbname_droptarget.js',
                'rename_collection_droptarget.js',
                'update_rename.js',
                'update_rename_noindex.js',
                'yield_sort.js',
            ].map(function(file) {
                return dir + '/' + file;
            });
            Random.setRandomSeed();
            // Run indefinitely.
            while (true) {
                try {
                    var workloads = Array.shuffle(ls(dir).filter(function(file) {
                        return !Array.contains(blacklist, file);
                    }));
                    // Run workloads one at a time, so we ensure replication completes.
                    workloads.forEach(function(workload) {
                        runWorkloadsSerially(
                            [workload], {}, {}, {dropDatabaseBlacklist: [blackListDb]});
                        // Wait for replication to complete between workloads.
                        var wc = {
                            writeConcern: {w: numNodes, wtimeout: ReplSetTest.kDefaultTimeoutMS}
                        };
                        var result = db.getSiblingDB('test').fsm_teardown.insert({a: 1}, wc);
                        assert.writeOK(result, 'teardown insert failed: ' + tojson(result));
                        result = db.getSiblingDB('test').fsm_teardown.drop();
                        assert(result, 'teardown drop failed');
                    });
                } catch (e) {
                    if (e instanceof ReferenceError || e instanceof TypeError) {
                        throw e;
                    }
                }
            }
        };

        // Returns the pid of the started mongo shell so the FSM test client can be terminated
        // without waiting for its execution to finish.
        return startMongoProgramNoConnect(
            'mongo',
            '--eval',
            '(' + fsmClientCmds + ')("' + blackListDb + '", ' + numNodes + ');',
            host);
    }

    /**
     * Runs the test.
     */
    this.run = function() {
        var options = this.options;

        jsTestLog("Backup restore " + tojson(options));

        // Test options
        // Test name
        var testName = jsTest.name();

        // Backup type (must be specified)
        var allowedBackupKeys = ['fsyncLock', 'stopStart', 'rolling'];
        assert(options.backup, "Backup option not supplied");
        assert.contains(options.backup,
                        allowedBackupKeys,
                        'invalid option: ' + tojson(options.backup) + '; valid options are: ' +
                            tojson(allowedBackupKeys));

        // Number of nodes in initial replica set (default 3)
        var numNodes = options.nodes || 3;

        // Time for clients to run before getting the new secondary it's data (default 10 seconds)
        var clientTime = options.clientTime || 10000;

        // Set the dbpath for the replica set
        var dbpathPrefix = MongoRunner.dataPath + 'backupRestore';
        resetDbpath(dbpathPrefix);
        var dbpathFormat = dbpathPrefix + '/mongod-$port';

        // Start numNodes node replSet
        var rst = new ReplSetTest({
            nodes: numNodes,
            nodeOptions: {dbpath: dbpathFormat},
            oplogSize: 1024,
        });
        var nodes = rst.startSet();

        // Initialize replica set using default timeout. This should give us sufficient time to
        // allocate 1GB oplogs on slow test hosts with mmapv1.
        rst.initiate();
        rst.awaitNodesAgreeOnPrimary();
        var primary = rst.getPrimary();
        var secondary = rst.getSecondary();

        // Launch CRUD client
        var crudDb = "crud";
        var crudColl = "backuprestore";
        var crudPid = _crudClient(primary.host, crudDb, crudColl);

        // Launch FSM client
        var fsmPid = _fsmClient(primary.host, crudDb, numNodes);

        // Let clients run for specified time before backing up secondary
        sleep(clientTime);

        // Perform fsync to create checkpoint. We doublecheck if the storage engine
        // supports fsync here.
        var ret = primary.adminCommand({fsync: 1});

        if (!ret.ok) {
            assert.commandFailedWithCode(ret, ErrorCodes.CommandNotSupported);
            jsTestLog('Skipping test of ' + options.backup + ' as it does not support fsync.');
            return;
        }

        // Configure new hidden secondary
        var dbpathSecondary = secondary.dbpath;
        var hiddenDbpath = dbpathPrefix + '/mongod-hiddensecondary';
        resetDbpath(hiddenDbpath);

        var sourcePath = dbpathSecondary + "/";
        var destPath = hiddenDbpath;
        // Windows paths for rsync
        if (_isWindows()) {
            sourcePath = "$(cygpath $(cygpath $SYSTEMDRIVE)'" + sourcePath + "')/";
            destPath = "$(cygpath $(cygpath $SYSTEMDRIVE)'" + hiddenDbpath + "')";
        }
        var copiedFiles;

        // Compare dbHash of crudDb when possible on hidden secondary
        var dbHash;

        // Perform the data backup to new secondary
        if (options.backup == 'fsyncLock') {
            // Test that the secondary supports fsyncLock
            var ret = secondary.getDB("admin").fsyncLock();
            if (!ret.ok) {
                assert.commandFailedWithCode(ret, ErrorCodes.CommandNotSupported);
                jsTestLog('Skipping test of ' + options.backup + ' as it does not support fsync.');
                return;
            }

            dbHash = secondary.getDB(crudDb).runCommand({dbhash: 1}).md5;
            copyDbpath(dbpathSecondary, hiddenDbpath);
            removeFile(hiddenDbpath + '/mongod.lock');
            print("Source directory:", tojson(ls(dbpathSecondary)));
            copiedFiles = ls(hiddenDbpath);
            print("Copied files:", tojson(copiedFiles));
            assert.gt(copiedFiles.length, 0, testName + ' no files copied');
            assert.commandWorked(secondary.getDB("admin").fsyncUnlock(),
                                 testName + ' failed to fsyncUnlock');
        } else if (options.backup == 'rolling') {
            var rsyncCmd = "rsync -aKkz --del " + sourcePath + " " + destPath;
            // Simulate a rolling rsync, do it 3 times before stopping process
            for (var i = 0; i < 3; i++) {
                _runCmd(rsyncCmd);
                sleep(10000);
            }
            // Stop the mongod process
            rst.stop(secondary.nodeId);
            // One final rsync
            _runCmd(rsyncCmd);
            removeFile(hiddenDbpath + '/mongod.lock');
            print("Source directory:", tojson(ls(dbpathSecondary)));
            copiedFiles = ls(hiddenDbpath);
            print("Copied files:", tojson(copiedFiles));
            assert.gt(copiedFiles.length, 0, testName + ' no files copied');
            rst.start(secondary.nodeId, {}, true);
        } else if (options.backup == 'stopStart') {
            // Stop the mongod process
            rst.stop(secondary.nodeId);
            copyDbpath(dbpathSecondary, hiddenDbpath);
            removeFile(hiddenDbpath + '/mongod.lock');
            print("Source directory:", tojson(ls(dbpathSecondary)));
            copiedFiles = ls(hiddenDbpath);
            print("Copied files:", tojson(copiedFiles));
            assert.gt(copiedFiles.length, 0, testName + ' no files copied');
            rst.start(secondary.nodeId, {}, true);
        }

        // Wait up to 5 minutes until restarted node is in state secondary.
        rst.waitForState(rst.getSecondaries(), ReplSetTest.State.SECONDARY);

        // Add new hidden node to replSetTest
        jsTestLog('Starting new hidden node (but do not add to replica set) with dbpath ' +
                  hiddenDbpath + '.');
        var nodesBeforeAddingHiddenMember = rst.nodes.slice();
        // ReplSetTest.add() will use default values for --oplogSize and --replSet consistent with
        // existing nodes.
        var hiddenCfg = {noCleanData: true, dbpath: hiddenDbpath};
        var hiddenNode = rst.add(hiddenCfg);
        var hiddenHost = hiddenNode.host;

        // Verify if dbHash is the same on hidden secondary for crudDb
        // Note the dbhash can only run when the DB is inactive to get a result
        // that can be compared, which is only in the fsyncLock/fsynUnlock case
        if (dbHash !== undefined) {
            assert.soon(function() {
                try {
                    // Need to hammer this since the node can disconnect connections as it is
                    // starting up into REMOVED replication state.
                    return (dbHash === hiddenNode.getDB(crudDb).runCommand({dbhash: 1}).md5);
                } catch (e) {
                    return false;
                }
            });
        }

        // Add new hidden secondary to replica set
        jsTestLog('Adding new hidden node ' + hiddenHost + ' to replica set.');
        rst.awaitNodesAgreeOnPrimary(ReplSetTest.kDefaultTimeoutMS, nodesBeforeAddingHiddenMember);
        primary = rst.getPrimary();
        var rsConfig = primary.getDB("local").system.replset.findOne();
        rsConfig.version += 1;
        var hiddenMember = {_id: numNodes, host: hiddenHost, priority: 0, hidden: true};
        rsConfig.members.push(hiddenMember);
        assert.commandWorked(primary.adminCommand({replSetReconfig: rsConfig}),
                             testName + ' failed to reconfigure replSet ' + tojson(rsConfig));

        // Wait up to 5 minutes until the new hidden node is in state RECOVERING.
        rst.waitForState(hiddenNode, [ReplSetTest.State.RECOVERING, ReplSetTest.State.SECONDARY]);

        // Stop CRUD client and FSM client.
        assert(checkProgram(crudPid), testName + ' CRUD client was not running at end of test');
        assert(checkProgram(fsmPid), testName + ' FSM client was not running at end of test');
        stopMongoProgramByPid(crudPid);
        stopMongoProgramByPid(fsmPid);

        // Wait up to 5 minutes until the new hidden node is in state SECONDARY.
        jsTestLog('CRUD and FSM clients stopped. Waiting for hidden node ' + hiddenHost +
                  ' to become SECONDARY');
        rst.waitForState(hiddenNode, ReplSetTest.State.SECONDARY);

        // Wait for secondaries to finish catching up before shutting down.
        jsTestLog(
            'Hidden node ' + hiddenHost +
            ' is now SECONDARY. Waiting for CRUD and FSM operations to be applied on all nodes.');
        rst.awaitReplication();

        jsTestLog('CRUD and FSM operations successfully applied on all nodes. ' +
                  'Waiting for all nodes to agree on the primary.');
        rst.awaitNodesAgreeOnPrimary();

        jsTestLog('All nodes agree on the primary. Getting current primary.');
        primary = rst.getPrimary();

        jsTestLog('Inserting single document into primary ' + primary.host +
                  ' with writeConcern w:' + rst.nodes.length);
        var writeResult = assert.writeOK(primary.getDB("test").foo.insert(
            {}, {writeConcern: {w: rst.nodes.length, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

        // Stop set.
        jsTestLog('Insert operation successful: ' + tojson(writeResult) +
                  '. Stopping replica set.');
        rst.stopSet();

        // Cleanup the files from the test
        // This is not done properly for replSetTest if dbpath is provided
        resetDbpath(dbpathPrefix);
        resetDbpath(hiddenDbpath);
    };
};
