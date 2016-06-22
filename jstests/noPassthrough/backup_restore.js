/**
 * Test the backup/restore process:
 * - 3 node replica set
 * - Mongo CRUD client
 * - Mongo FSM client
 * - fsyncLock (or stop) Secondary
 * - cp (or rsync) DB files
 * - fsyncUnlock (or start) Secondary
 * - Start mongod as hidden secondary
 * - Wait until new hidden node becomes secondary
 *
 * Some methods for backup used in this test checkpoint the files in the dbpath. This technique will
 * not work for ephemeral storage engines, as they do not store any data in the dbpath.
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    function runCmd(cmd) {
        runProgram('bash', '-c', cmd);
    }

    function crudClient(host, dbName, coll) {
        // Launch CRUD client
        var crudClientCmds = "var bulkNum = 1000;" + "var baseNum = 100000;" +
            "var coll = db.getSiblingDB('" + dbName + "')." + coll + ";" +
            "coll.ensureIndex({x: 1});" + "var largeValue = new Array(1024).join('L');" +
            "Random.setRandomSeed();" +
            // run indefinitely
            "while (true) {" + "   try {" + "       var op = Random.rand();" +
            "       var match = Math.floor(Random.rand() * baseNum);" + "       if (op < 0.2) {" +
            // 20% of the operations: bulk insert bulkNum docs
            "           var bulk = coll.initializeUnorderedBulkOp();" +
            "           for (var i = 0; i < bulkNum; i++) {" +
            "               bulk.insert({x: (match * i) % baseNum," +
            "                   doc: largeValue.substring(0, match % largeValue.length)});" +
            "           }" + "           assert.writeOK(bulk.execute());" +
            "       } else if (op < 0.4) {" +
            // 20% of the operations: update docs;
            "           var updateOpts = {upsert: true, multi: true};" +
            "           assert.writeOK(coll.update(" + "               {x: {$gte: match}}," +
            "               {$inc: {x: baseNum}, $set: {n: 'hello'}}," +
            "               updateOpts));" + "       } else if (op < 0.9) {" +
            // 50% of the operations: find matchings docs
            // itcount() consumes the cursor
            "           coll.find({x: {$gte: match}}).itcount();" + "       } else {" +
            // 10% of the operations: remove matching docs
            "           assert.writeOK(coll.remove({x: {$gte: match}}));" + "       }" +
            "   } catch(e) {" +
            "       if (e instanceof ReferenceError || e instanceof TypeError) {" +
            "           throw e;" + "       }" + "   }" + "}";

        // Returns the pid of the started mongo shell so the CRUD test client can be terminated
        // without waiting for its execution to finish.
        return startMongoProgramNoConnect("mongo", "--eval", crudClientCmds, host);
    }

    function fsmClient(host, blackListDb, numNodes) {
        // Launch FSM client
        // SERVER-19488 The FSM framework assumes that there is an implicit 'db' connection when
        // started without any cluster options. Since the shell running this test was started with
        // --nodb, another mongo shell is used to allow implicit connections to be made to the
        // primary of the replica set.
        var fsmClientCmds = "'use strict';" + "load('jstests/concurrency/fsm_libs/runner.js');" +
            "var dir = 'jstests/concurrency/fsm_workloads';" + "var blacklist = [" +
            "    'agg_group_external.js'," + "    'agg_sort_external.js'," +
            "    'auth_create_role.js'," + "    'auth_create_user.js'," +
            "    'auth_drop_role.js'," + "    'auth_drop_user.js'," +
            "    'reindex_background.js'," + "    'yield_sort.js'," +
            "    'create_index_background.js'," +
            "].map(function(file) { return dir + '/' + file; });" + "Random.setRandomSeed();" +
            // run indefinitely
            "while (true) {" + "   try {" +
            "       var workloads = Array.shuffle(ls(dir).filter(function(file) {" +
            "           return !Array.contains(blacklist, file);" + "       }));" +
            // Run workloads one at a time, so we ensure replication completes
            "       workloads.forEach(function(workload) {" +
            "           runWorkloadsSerially([workload]," +
            "               {}, {}, {dropDatabaseBlacklist: ['" + blackListDb + "']});" +
            // Wait for replication to complete between workloads
            "           var wc = {writeConcern: {w: " + numNodes + ", wtimeout: 300000}};" +
            "           var result = db.getSiblingDB('test').fsm_teardown.insert({ a: 1 }, wc);" +
            "           assert.writeOK(result, 'teardown insert failed: ' + tojson(result));" +
            "           result = db.getSiblingDB('test').fsm_teardown.drop();" +
            "           assert(result, 'teardown drop failed');" + "       });" +
            "   } catch(e) {" +
            "       if (e instanceof ReferenceError || e instanceof TypeError) {" +
            "           throw e;" + "       }" + "   }" + "}";

        // Returns the pid of the started mongo shell so the FSM test client can be terminated
        // without waiting for its execution to finish.
        return startMongoProgramNoConnect("mongo", "--eval", fsmClientCmds, host);
    }

    function runTest(options) {
        jsTestLog("Backup restore " + tojson(options));

        // Test options
        // Test name
        assert(options.name, 'Test name option not supplied');
        var testName = options.name;

        // Storage engine being tested
        var storageEngine = options.storageEngine;

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
        var replSetName = 'backupRestore';
        var rst = new ReplSetTest({
            name: replSetName,
            nodes: numNodes,
            nodeOptions: {oplogSize: 1024, storageEngine: storageEngine, dbpath: dbpathFormat}
        });
        var nodes = rst.startSet();

        // Wait up to 5 minutes for the replica set to initiate. We allow extra time because
        // allocating 1GB oplogs on test hosts can be slow with mmapv1.
        rst.initiate(null, null, 5 * 60 * 1000);
        var primary = rst.getPrimary();
        var secondary = rst.getSecondary();

        // Launch CRUD client
        var crudDb = "crud";
        var crudColl = "backuprestore";
        var crudPid = crudClient(primary.host, crudDb, crudColl);

        // Launch FSM client
        var fsmPid = fsmClient(primary.host, crudDb, numNodes);

        // Let clients run for specified time before backing up secondary
        sleep(clientTime);

        // Perform fsync to create checkpoint. We doublecheck if the storage engine
        // supports fsync here.
        var ret = primary.adminCommand({fsync: 1});

        if (!ret.ok) {
            assert.commandFailedWithCode(ret, ErrorCodes.CommandNotSupported);
            jsTestLog("Skipping test of " + options.backup + " for " + storageEngine +
                      ' as it does not support fsync');
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
                jsTestLog("Skipping test of " + options.backup + " for " + storageEngine +
                          ' as it does not support fsync');
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
                runCmd(rsyncCmd);
                sleep(10000);
            }
            // Stop the mongod process
            rst.stop(secondary.nodeId);
            // One final rsync
            runCmd(rsyncCmd);
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

        // Wait up to 60 seconds until restarted node is in state secondary
        rst.waitForState(rst.getSecondaries(), ReplSetTest.State.SECONDARY, 60 * 1000);

        // Add new hidden node to replSetTest
        var hiddenCfg =
            {restart: true, oplogSize: 1024, dbpath: hiddenDbpath, replSet: replSetName};
        rst.add(hiddenCfg);
        var hiddenHost = rst.nodes[numNodes].host;

        // Verify if dbHash is the same on hidden secondary for crudDb
        // Note the dbhash can only run when the DB is inactive to get a result
        // that can be compared, which is only in the fsyncLock/fsynUnlock case
        if (dbHash !== undefined) {
            assert.soon(function() {
                try {
                    // Need to hammer this since the node can disconnect connections as it is
                    // starting up into REMOVED replication state.
                    return (dbHash ===
                            rst.nodes[numNodes].getDB(crudDb).runCommand({dbhash: 1}).md5);
                } catch (e) {
                    return false;
                }
            });
        }

        // Add new hidden secondary to replica set
        var rsConfig = primary.getDB("local").system.replset.findOne();
        rsConfig.version += 1;
        var hiddenMember = {_id: numNodes, host: hiddenHost, priority: 0, hidden: true};
        rsConfig.members.push(hiddenMember);
        assert.commandWorked(primary.adminCommand({replSetReconfig: rsConfig}),
                             testName + ' failed to reconfigure replSet ' + tojson(rsConfig));

        // Wait up to 60 seconds until the new hidden node is in state RECOVERING.
        rst.waitForState(rst.nodes[numNodes],
                         [ReplSetTest.State.RECOVERING, ReplSetTest.State.SECONDARY],
                         60 * 1000);

        // Stop CRUD client and FSM client.
        assert(checkProgram(crudPid), testName + ' CRUD client was not running at end of test');
        assert(checkProgram(fsmPid), testName + ' FSM client was not running at end of test');
        stopMongoProgramByPid(crudPid);
        stopMongoProgramByPid(fsmPid);

        // Wait up to 60 seconds until the new hidden node is in state SECONDARY.
        rst.waitForState(rst.nodes[numNodes], ReplSetTest.State.SECONDARY, 60 * 1000);

        // Wait for secondaries to finish catching up before shutting down.
        assert.writeOK(primary.getDB("test").foo.insert(
            {}, {writeConcern: {w: rst.nodes.length, wtimeout: 10 * 60 * 1000}}));

        // Stop set.
        rst.stopSet();

        // Cleanup the files from the test
        // This is not done properly for replSetTest if dbpath is provided
        resetDbpath(dbpathPrefix);
        resetDbpath(hiddenDbpath);
    }

    // Main

    // Add storage engines which are to be skipped entirely to this array
    var noBackupTests = ['inMemoryExperiment'];

    // Grab the storage engine, default is wiredTiger
    var storageEngine = jsTest.options().storageEngine || "wiredTiger";

    if (noBackupTests.indexOf(storageEngine) != -1) {
        jsTestLog("Skipping test for " + storageEngine);
        return;
    }

    if (storageEngine === "wiredTiger") {
        // if rsync is not available on the host, then this test is skipped
        if (!runProgram('bash', '-c', 'which rsync')) {
            runTest({
                name: storageEngine + ' rolling',
                storageEngine: storageEngine,
                backup: 'rolling',
                clientTime: 30000
            });
        } else {
            jsTestLog("Skipping test for " + storageEngine + ' rolling');
        }
    }

    // Run the fsyncLock test. Will return before testing for any engine that doesn't
    // support fsyncLock
    runTest({
        name: storageEngine + ' fsyncLock/fsyncUnlock',
        storageEngine: storageEngine,
        backup: 'fsyncLock'
    });

    runTest({
        name: storageEngine + ' stop/start',
        storageEngine: storageEngine,
        backup: 'stopStart',
        clientTime: 30000
    });

}());
