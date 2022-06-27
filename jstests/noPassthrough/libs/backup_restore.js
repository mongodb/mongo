/**
 * Sets up a test for the backup/restore process:
 * - 3 node replica set
 * - Mongo CRUD client
 * - Mongo FSM client
 * - fsyncLock, stop or open a backupCursor on a Secondary
 * - cp (or rsync) DB files
 * - fsyncUnlock, start or close a backupCursor on the Secondary
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
    function _crudClient(host, dbName, collectionName, numNodes) {
        // Launch CRUD client
        var crudClientCmds = function(dbName, collectionName, numNodes) {
            var bulkNum = 1000;
            var baseNum = 100000;

            let iteration = 0;

            var coll = db.getSiblingDB(dbName).getCollection(collectionName);
            coll.createIndex({x: 1});

            var largeValue = new Array(1024).join('L');

            Random.setRandomSeed();

            // Run indefinitely.
            while (true) {
                ++iteration;

                // We periodically use a write concern of w='numNodes' as a backpressure mechanism
                // to prevent the secondaries from falling off the primary's oplog. The CRUD client
                // inserts ~1KB documents 1000 at a time, so in the worst case we'll have rolled the
                // primary's oplog over every ~1000 iterations. We use 100 iterations for the
                // frequency of when to use a write concern of w='numNodes' to lessen the risk of
                // being unlucky as a result of running concurrently with the FSM client. Note that
                // although the updates performed by the CRUD client may in the worst case modify
                // every document, the oplog entries produced as a result are 10x smaller than the
                // document itself.
                const writeConcern = (iteration % 100 === 0) ? {w: numNodes} : {w: 1};

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
                        assert.commandWorked(bulk.execute(writeConcern));
                    } else if (op < 0.4) {
                        // 20% of the operations: update docs.
                        var updateOpts = {upsert: true, multi: true, writeConcern: writeConcern};
                        assert.commandWorked(coll.update({x: {$gte: match}},
                                                         {$inc: {x: baseNum}, $set: {n: 'hello'}},
                                                         updateOpts));
                    } else if (op < 0.9) {
                        // 50% of the operations: find matchings docs.
                        // itcount() consumes the cursor
                        coll.find({x: {$gte: match}}).itcount();
                    } else {
                        // 10% of the operations: remove matching docs.
                        assert.commandWorked(
                            coll.remove({x: {$gte: match}}, {writeConcern: writeConcern}));
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
        return startMongoProgramNoConnect(MongoRunner.mongoShellPath,
                                          '--eval',
                                          '(' + crudClientCmds + ')("' + dbName + '", "' +
                                              collectionName + '", ' + numNodes + ')',
                                          host);
    }

    /**
     * Starts a client that will run a FSM workload.
     */
    function _fsmClient(host) {
        // Launch FSM client
        const suite = 'concurrency_replication_for_backup_restore';
        const resmokeCmd = 'python buildscripts/resmoke.py run --shuffle --continueOnFailure' +
            ' --repeat=99999 --internalParam=is_inner_level --mongo=' + MongoRunner.mongoShellPath +
            ' --shellConnString=mongodb://' + host + ' --suites=' + suite;

        // Returns the pid of the FSM test client so it can be terminated without waiting for its
        // execution to finish.
        return _startMongoProgram({args: resmokeCmd.split(' ')});
    }

    /**
     * Runs the test.
     */
    this.run = function() {
        var options = this.options;

        jsTestLog("Backup restore " + tojson(options));

        // skipValidationOnNamespaceNotFound must be set to true for correct operation of this test.
        assert(typeof TestData.skipValidationOnNamespaceNotFound === 'undefined' ||
               TestData.skipValidationOnNamespaceNotFound);

        // Test options
        // Test name
        var testName = jsTest.name();

        // Backup type (must be specified)
        var allowedBackupKeys = ['fsyncLock', 'stopStart', 'rolling', 'backupCursor'];
        assert(options.backup, "Backup option not supplied");
        assert.contains(options.backup,
                        allowedBackupKeys,
                        'invalid option: ' + tojson(options.backup) +
                            '; valid options are: ' + tojson(allowedBackupKeys));

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
            nodeOptions: {
                dbpath: dbpathFormat,
                setParameter: {logComponentVerbosity: tojsononeline({storage: {recovery: 2}})}
            },
            oplogSize: 1024
        });

        // Avoid stepdowns due to heavy workloads on slow machines.
        var config = rst.getReplSetConfig();
        config.settings = {electionTimeoutMillis: 60000};
        var nodes = rst.startSet();
        rst.initiate(config);

        // Initialize replica set using default timeout. This should give us sufficient time to
        // allocate 1GB oplogs on slow test hosts.
        rst.awaitNodesAgreeOnPrimary();
        var primary = rst.getPrimary();
        var secondary = rst.getSecondary();

        jsTestLog("Secondary to copy data from: " + secondary);

        // Launch CRUD client
        var crudDb = "crud";
        var crudColl = "backuprestore";
        var crudPid = _crudClient(primary.host, crudDb, crudColl, numNodes);

        // Launch FSM client
        var fsmPid = _fsmClient(primary.host);

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

        // Perform the data backup to new secondary
        if (options.backup == 'fsyncLock') {
            rst.awaitSecondaryNodes();
            // Test that the secondary supports fsyncLock
            var ret = secondary.getDB("admin").fsyncLock();
            if (!ret.ok) {
                assert.commandFailedWithCode(ret, ErrorCodes.CommandNotSupported);
                jsTestLog('Skipping test of ' + options.backup + ' as it does not support fsync.');
                return;
            }

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
        } else if (options.backup == 'backupCursor') {
            load("jstests/libs/backup_utils.js");

            backupData(secondary, hiddenDbpath);
            copiedFiles = ls(hiddenDbpath);
            jsTestLog("Copying End: " + tojson({
                          destinationFiles: copiedFiles,
                          destinationJournal: ls(hiddenDbpath + '/journal')
                      }));
            assert.gt(copiedFiles.length, 0, testName + ' no files copied');
        }

        // Wait up to 5 minutes until restarted node is in state secondary.
        rst.waitForState(rst.getSecondaries(), ReplSetTest.State.SECONDARY);

        jsTestLog('Stopping CRUD and FSM clients');

        // Stop CRUD client and FSM client.
        var crudStatus = checkProgram(crudPid);
        assert(crudStatus.alive,
               testName + ' CRUD client was not running at end of test and exited with code: ' +
                   crudStatus.exitCode);
        stopMongoProgramByPid(crudPid);

        var fsmStatus = checkProgram(fsmPid);
        assert(fsmStatus.alive,
               testName + ' FSM client was not running at end of test and exited with code: ' +
                   fsmStatus.exitCode);

        const kSIGINT = 2;
        const exitCode = stopMongoProgramByPid(fsmPid, kSIGINT);
        if (!_isWindows()) {
            // The mongo shell calls TerminateProcess() on Windows rather than more gracefully
            // interrupting resmoke.py test execution.

            // resmoke.py may exit cleanly on SIGINT returning 130, or uncleanly in which case
            // stopMongoProgramByPid returns -SIGINT.
            assert(exitCode == 130 || exitCode == -kSIGINT,
                   'expected resmoke.py to exit due to being interrupted');
        }

        // Make sure the databases are not in a drop-pending state. This can happen if we
        // killed the FSM client while it was in the middle of dropping them.
        let result = primary.adminCommand({
            listDatabases: 1,
            nameOnly: true,
            filter: {'name': {$nin: ['admin', 'config', 'local', '$external']}}
        });
        assert.commandWorked(result);

        // We use the implicitly_retry_on_database_drop_pending.js override file to
        // handle the retry logic for DatabaseDropPending error responses.
        load("jstests/libs/override_methods/implicitly_retry_on_database_drop_pending.js");

        const databases = result.databases.map(dbs => dbs.name);
        databases.forEach(dbName => {
            assert.commandWorked(primary.getDB(dbName).afterClientKills.insert(
                {'a': 1}, {writeConcern: {w: 'majority'}}));
        });

        // Add the new hidden node to replSetTest.
        jsTestLog('Starting new hidden node (but do not add to replica set) with dbpath ' +
                  hiddenDbpath + '.');
        var nodesBeforeAddingHiddenMember = rst.nodes.slice();
        // ReplSetTest.add() will use default values for --oplogSize and --replSet consistent with
        // existing nodes.
        var hiddenCfg = {noCleanData: true, dbpath: hiddenDbpath};
        var hiddenNode = rst.add(hiddenCfg);
        var hiddenHost = hiddenNode.host;

        // Add the new hidden secondary to the replica set. This triggers an election, so it must be
        // done after stopping the background workloads to prevent the workloads from failing if a
        // new primary is elected.
        jsTestLog('Adding new hidden node ' + hiddenHost + ' to replica set.');
        rst.awaitNodesAgreeOnPrimary(ReplSetTest.kDefaultTimeoutMS, nodesBeforeAddingHiddenMember);
        primary = rst.getPrimary();
        var rsConfig = primary.getDB("local").system.replset.findOne();
        rsConfig.version += 1;
        var hiddenMember = {_id: numNodes, host: hiddenHost, priority: 0, hidden: true};
        rsConfig.members.push(hiddenMember);
        assert.commandWorked(primary.adminCommand({replSetReconfig: rsConfig}),
                             testName + ' failed to reconfigure replSet ' + tojson(rsConfig));

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
        var writeResult = assert.commandWorked(primary.getDB("test").foo.insert(
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
