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

// We use the implicitly_retry_on_database_drop_pending.js override file to
// handle the retry logic for DatabaseDropPending error responses.
import "jstests/libs/override_methods/implicitly_retry_on_database_drop_pending.js";

import {backupData} from "jstests/libs/backup_utils.js";
import {getPython3Binary} from "jstests/libs/python.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

export const BackupRestoreTest = function (options) {
    if (!(this instanceof BackupRestoreTest)) {
        return new BackupRestoreTest(options);
    }

    // Capture the 'this' reference
    let self = this;

    self.options = options;

    /**
     * Runs a command in the bash shell.
     */
    function _runCmd(cmd) {
        runProgram("bash", "-c", cmd);
    }

    /**
     * Starts a client that will run a CRUD workload.
     */
    function _crudClient(host, dbName, collectionName, numNodes) {
        // Launch CRUD client
        let crudClientCmds = function (dbName, collectionName, numNodes) {
            let bulkNum = 1000;
            let baseNum = 100000;

            let iteration = 0;

            let coll = db.getSiblingDB(dbName).getCollection(collectionName);
            coll.createIndex({x: 1});

            let largeValue = "L".repeat(1023);

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
                const writeConcern = iteration % 100 === 0 ? {w: numNodes} : {w: 1};

                try {
                    let op = Random.rand();
                    let match = Math.floor(Random.rand() * baseNum);
                    if (op < 0.2) {
                        // 20% of the operations: bulk insert bulkNum docs.
                        let bulk = coll.initializeUnorderedBulkOp();
                        for (let i = 0; i < bulkNum; i++) {
                            bulk.insert({
                                x: (match * i) % baseNum,
                                doc: largeValue.substring(0, match % largeValue.length),
                            });
                        }
                        assert.commandWorked(bulk.execute(writeConcern));
                    } else if (op < 0.4) {
                        // 20% of the operations: update docs.
                        let updateOpts = {upsert: true, multi: true, writeConcern: writeConcern};
                        assert.commandWorked(
                            coll.update({x: {$gte: match}}, {$inc: {x: baseNum}, $set: {n: "hello"}}, updateOpts),
                        );
                    } else if (op < 0.9) {
                        // 50% of the operations: find matchings docs.
                        // itcount() consumes the cursor
                        coll.find({x: {$gte: match}}).itcount();
                    } else {
                        // 10% of the operations: remove matching docs.
                        assert.commandWorked(coll.remove({x: {$gte: match}}, {writeConcern: writeConcern}));
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
        let shellPath = MongoRunner.getMongoShellPath();
        return startMongoProgramNoConnect(
            shellPath,
            "--eval",
            "(" + crudClientCmds + ')("' + dbName + '", "' + collectionName + '", ' + numNodes + ")",
            host,
        );
    }

    /**
     * Starts a client that will run a FSM workload.
     */
    function _fsmClient(host) {
        // Launch FSM client
        const suite = "concurrency_replication_for_backup_restore";
        const resmokeCmd =
            getPython3Binary() +
            " buildscripts/resmoke.py run --shuffle --continueOnFailure" +
            " --repeat=99999 --internalParam=is_inner_level --mongo=" +
            MongoRunner.getMongoShellPath() +
            " --shellConnString=mongodb://" +
            host +
            " --suites=" +
            suite;

        // Returns the pid of the FSM test client so it can be terminated without waiting for its
        // execution to finish.
        return _startMongoProgram({args: resmokeCmd.split(" ")});
    }

    /**
     * Runs the test.
     */
    this.run = function () {
        let options = this.options;

        jsTestLog("Backup restore " + tojson(options));

        // skipValidationOnNamespaceNotFound must be set to true for correct operation of this test.
        assert(
            typeof TestData.skipValidationOnNamespaceNotFound === "undefined" ||
                TestData.skipValidationOnNamespaceNotFound,
        );

        // Test options
        // Test name
        let testName = jsTest.name();

        // Backup type (must be specified)
        let allowedBackupKeys = ["fsyncLock", "stopStart", "rolling", "backupCursor"];
        assert(options.backup, "Backup option not supplied");
        assert.contains(
            options.backup,
            allowedBackupKeys,
            "invalid option: " + tojson(options.backup) + "; valid options are: " + tojson(allowedBackupKeys),
        );

        // Number of nodes in initial replica set (default 3)
        let numNodes = options.nodes || 3;

        // Time for clients to run before getting the new secondary it's data (default 10 seconds)
        let clientTime = options.clientTime || 10000;

        // Set the dbpath for the replica set
        let dbpathPrefix = MongoRunner.dataPath + "backupRestore";
        resetDbpath(dbpathPrefix);
        let dbpathFormat = dbpathPrefix + "/mongod-$port";

        // Start numNodes node replSet
        let rst = new ReplSetTest({
            nodes: numNodes,
            nodeOptions: {
                dbpath: dbpathFormat,
                setParameter: {logComponentVerbosity: tojsononeline({storage: {recovery: 2}})},
            },
            oplogSize: 1024,
        });

        // Avoid stepdowns due to heavy workloads on slow machines.
        let config = rst.getReplSetConfig();
        config.settings = {electionTimeoutMillis: 60000};
        let nodes = rst.startSet();
        rst.initiate(config);

        // Initialize replica set using default timeout. This should give us sufficient time to
        // allocate 1GB oplogs on slow test hosts.
        rst.awaitNodesAgreeOnPrimary();
        let primary = rst.getPrimary();
        let secondary = rst.getSecondary();

        jsTestLog("Secondary to copy data from: " + secondary);

        // Launch CRUD client
        let crudDb = "crud";
        let crudColl = "backuprestore";
        let crudPid = _crudClient(primary.host, crudDb, crudColl, numNodes);

        // Launch FSM client
        let fsmPid = 0;
        let attempts = 0;

        // Make sure the fsm client does not fail within the first few seconds of running due to
        // some unrelated network error (usually when fetching yml files from a remote location).
        do {
            jsTestLog("Attempt " + attempts + " of starting up the fsm client");
            fsmPid = _fsmClient(primary.host);
            attempts += 1;
            sleep(5 * 1000);
        } while (!checkProgram(fsmPid).alive && attempts < 10);

        // Let clients run for specified time before backing up secondary
        sleep(clientTime);

        // Perform fsync to create checkpoint. We doublecheck if the storage engine
        // supports fsync here.
        let ret = primary.adminCommand({fsync: 1});

        if (!ret.ok) {
            assert.commandFailedWithCode(ret, ErrorCodes.CommandNotSupported);
            jsTestLog("Skipping test of " + options.backup + " as it does not support fsync.");
            return;
        }

        // Configure new hidden secondary
        let dbpathSecondary = secondary.dbpath;
        let hiddenDbpath = dbpathPrefix + "/mongod-hiddensecondary";
        resetDbpath(hiddenDbpath);

        let sourcePath = dbpathSecondary + "/";
        let destPath = hiddenDbpath;
        // Windows paths for rsync
        if (_isWindows()) {
            sourcePath = "$(cygpath $(cygpath $SYSTEMDRIVE)'" + sourcePath + "')/";
            destPath = "$(cygpath $(cygpath $SYSTEMDRIVE)'" + hiddenDbpath + "')";
        }
        let copiedFiles;

        // Perform the data backup to new secondary
        if (options.backup == "fsyncLock") {
            rst.awaitSecondaryNodes();
            // Test that the secondary supports fsyncLock
            let ret = secondary.getDB("admin").fsyncLock();
            if (!ret.ok) {
                assert.commandFailedWithCode(ret, ErrorCodes.CommandNotSupported);
                jsTestLog("Skipping test of " + options.backup + " as it does not support fsync.");
                return;
            }

            copyDbpath(dbpathSecondary, hiddenDbpath);
            removeFile(hiddenDbpath + "/mongod.lock");
            print("Source directory:", tojson(ls(dbpathSecondary)));
            copiedFiles = ls(hiddenDbpath);
            print("Copied files:", tojson(copiedFiles));
            assert.gt(copiedFiles.length, 0, testName + " no files copied");
            assert.commandWorked(secondary.getDB("admin").fsyncUnlock(), testName + " failed to fsyncUnlock");
        } else if (options.backup == "rolling") {
            let rsyncCmd = "rsync -aKkz --del " + sourcePath + " " + destPath;
            // Simulate a rolling rsync, do it 3 times before stopping process
            for (let i = 0; i < 3; i++) {
                _runCmd(rsyncCmd);
                sleep(10000);
            }

            // Stop the mongod process
            rst.stop(secondary.nodeId);

            // One final rsync
            _runCmd(rsyncCmd);
            removeFile(hiddenDbpath + "/mongod.lock");
            print("Source directory:", tojson(ls(dbpathSecondary)));
            copiedFiles = ls(hiddenDbpath);
            print("Copied files:", tojson(copiedFiles));
            assert.gt(copiedFiles.length, 0, testName + " no files copied");
            rst.start(secondary.nodeId, {}, true);
        } else if (options.backup == "stopStart") {
            // Stop the mongod process
            rst.stop(secondary.nodeId);

            copyDbpath(dbpathSecondary, hiddenDbpath);
            removeFile(hiddenDbpath + "/mongod.lock");
            print("Source directory:", tojson(ls(dbpathSecondary)));
            copiedFiles = ls(hiddenDbpath);
            print("Copied files:", tojson(copiedFiles));
            assert.gt(copiedFiles.length, 0, testName + " no files copied");
            rst.start(secondary.nodeId, {}, true);
        } else if (options.backup == "backupCursor") {
            backupData(secondary, hiddenDbpath);
            copiedFiles = ls(hiddenDbpath);
            jsTestLog(
                "Copying End: " +
                    tojson({
                        destinationFiles: copiedFiles,
                        destinationJournal: ls(hiddenDbpath + "/journal"),
                    }),
            );
            assert.gt(copiedFiles.length, 0, testName + " no files copied");
        }

        // Wait up to 5 minutes until restarted node is in state secondary.
        rst.awaitSecondaryNodes();

        jsTestLog("Stopping CRUD and FSM clients");

        // Stop CRUD client and FSM client.
        let crudStatus = checkProgram(crudPid);
        assert(
            crudStatus.alive,
            testName + " CRUD client was not running at end of test and exited with code: " + crudStatus.exitCode,
        );
        stopMongoProgramByPid(crudPid);

        const fsmStatus = checkProgram(fsmPid);
        // If the fsmClient ran successfully then kill it, otherwise log that it was not running and
        // move on. This is okay since the fsm client would have failed for reasons unrelated to the
        // restore procedure.
        if (fsmStatus.alive) {
            const kSIGINT = 2;
            const exitCode = stopMongoProgramByPid(fsmPid, kSIGINT);
            if (!_isWindows()) {
                // The mongo shell calls TerminateProcess() on Windows rather than more gracefully
                // interrupting resmoke.py test execution.

                // resmoke.py may exit cleanly on SIGINT, returning 130 if the suite tests were
                // running and returning SIGINT otherwise. It may also exit uncleanly, in which case
                // stopMongoProgramByPid returns -SIGINT. See SERVER-67390 and SERVER-72449.
                assert(
                    exitCode == 130 || exitCode == -kSIGINT || exitCode == kSIGINT,
                    "expected resmoke.py to exit due to being interrupted, but exited with code: " + exitCode,
                );
            }
        } else {
            jsTestLog(
                testName + " FSM client was not running at end of test and exited with code: " + fsmStatus.exitCode,
            );
        }

        // Make sure the databases are not in a drop-pending state. This can happen if we
        // killed the FSM client while it was in the middle of dropping them.
        let result = primary.adminCommand({
            listDatabases: 1,
            nameOnly: true,
            filter: {"name": {$nin: ["admin", "config", "local", "$external"]}},
        });
        assert.commandWorked(result);

        const databases = result.databases.map((dbs) => dbs.name);
        databases.forEach((dbName) => {
            assert.commandWorked(
                primary.getDB(dbName).afterClientKills.insert({"a": 1}, {writeConcern: {w: "majority"}}),
            );
        });

        // Add the new hidden node to replSetTest.
        jsTestLog("Starting new hidden node (but do not add to replica set) with dbpath " + hiddenDbpath + ".");
        let nodesBeforeAddingHiddenMember = rst.nodes.slice();
        // ReplSetTest.add() will use default values for --oplogSize and --replSet consistent with
        // existing nodes.
        let hiddenCfg = {noCleanData: true, dbpath: hiddenDbpath};
        let hiddenNode = rst.add(hiddenCfg);
        let hiddenHost = hiddenNode.host;

        // Add the new hidden secondary to the replica set. This triggers an election, so it must be
        // done after stopping the background workloads to prevent the workloads from failing if a
        // new primary is elected.
        jsTestLog("Adding new hidden node " + hiddenHost + " to replica set.");
        rst.awaitNodesAgreeOnPrimary(ReplSetTest.kDefaultTimeoutMS, nodesBeforeAddingHiddenMember);
        primary = rst.getPrimary();
        let rsConfig = primary.getDB("local").system.replset.findOne();
        rsConfig.version += 1;
        let hiddenMember = {_id: numNodes, host: hiddenHost, priority: 0, hidden: true};
        rsConfig.members.push(hiddenMember);
        assert.commandWorked(
            primary.adminCommand({replSetReconfig: rsConfig}),
            testName + " failed to reconfigure replSet " + tojson(rsConfig),
        );

        // Wait up to 5 minutes until the new hidden node is in state SECONDARY.
        jsTestLog("CRUD and FSM clients stopped. Waiting for hidden node " + hiddenHost + " to become SECONDARY");
        rst.awaitSecondaryNodes(null, [hiddenNode]);

        // Wait for secondaries to finish catching up before shutting down.
        jsTestLog(
            "Hidden node " +
                hiddenHost +
                " is now SECONDARY. Waiting for CRUD and FSM operations to be applied on all nodes.",
        );
        rst.awaitReplication();

        jsTestLog(
            "CRUD and FSM operations successfully applied on all nodes. " +
                "Waiting for all nodes to agree on the primary.",
        );
        rst.awaitNodesAgreeOnPrimary();

        jsTestLog("All nodes agree on the primary. Getting current primary.");
        primary = rst.getPrimary();

        jsTestLog(
            "Inserting single document into primary " + primary.host + " with writeConcern w:" + rst.nodes.length,
        );
        let writeResult = assert.commandWorked(
            primary
                .getDB("test")
                .foo.insert({}, {writeConcern: {w: rst.nodes.length, wtimeout: ReplSetTest.kDefaultTimeoutMS}}),
        );

        // Stop set.
        jsTestLog("Insert operation successful: " + tojson(writeResult) + ". Stopping replica set.");
        rst.stopSet();

        // Cleanup the files from the test
        // This is not done properly for replSetTest if dbpath is provided
        resetDbpath(dbpathPrefix);
        resetDbpath(hiddenDbpath);
    };
};
