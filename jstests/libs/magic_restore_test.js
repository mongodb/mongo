import * as backupUtils from "jstests/libs/backup_utils.js";
/**
 * This class implements helpers for testing the magic restore process. It wraps a ReplSetTest
 * object and maintains the state of the backup cursor, handles writing objects to named pipes and
 * running the restore process. It exposes some of this state so that individual tests can make
 * specific assertions as needed.
 *
 * @class
 */
export class MagicRestoreTest {
    /**
     * Creates a new MagicRestoreTest instance.
     *
     * @constructor
     * @param {Object} [params] The parameters object for the MagicRestoreTest.
     * @param {Object} [params.rst] The ReplSetTest object.
     * @param {string} [params.pipeDir] The file path of the named pipe. The pipe is used by magic
     *     restore to read the restore configuration and any additional PIT oplog entries. This
     *     should usually be 'MongoRunner.dataDir'.
     * @param {boolean} [params.insertHigherTermOplogEntry=false] Whether to insert a higher-term
     *     oplog entry during the restore procedure. This is used by Cloud to maintain MongoDB
     *     driver connections to a node after restore.
     *
     * @property {Object} [backupSource] The connection object to the node used for backup. We
     * perform backup and restore for all nodes in a replica set using one set of data files.
     * @property {string} [backupDbPath] The file path used to store backed up data files. We'll
     * copy these data files into separate dbpaths for each node.
     * @property {Array.<string>} [restoreDbPaths] The file paths for each restored node's data
     * files. Each dbpath ends with '/restore_${nodeId}'.
     * @property {boolean} [isPit] Whether or not this particular restore is a point-in-time
     * restore.
     * @property {boolean} [insertHigherTermOplogEntry] Whether to insert a higher-term
     * oplog entry during the restore procedure. This is used by Cloud to maintain MongoDB
     * driver connections to a node after restore.
     * @property {number} [restoreToHigherTermThan] Default higher term value to pass into magic
     * restore when 'insertHigherTermOplogEntry' is set to true.
     * @property {Object} [expectedConfig] The expected replica set config after the restore. Used
     * to make assertions about a node post-restore.
     * @property {Object} [backupCursor] The WiredTiger backup cursor object.
     * @property {number} [backupId] The WiredTiger backup cursor ID.
     * @property {Object} [checkpointTimestamp] The checkpoint timestamp from the WiredTiger backup
     * cursor.
     * @property {Object} [pointInTimeTimestamp] The timestamp to restore to with additional oplog
     * entries in a PIT restore.
     * @property {Array.<Object>} [collectionsToRestore] A list of objects containing namespace and
     * UUID pairs. Used to perform a selective restore.
     * @property {Object} [preRestoreDbHashes] An object containing database and collection hashes
     * before restore. Used to compare on-disk data before and after restore. Produces an object
     * with the shape:
     * {
     *     "db0": {
     *         "coll0": <hash>,
     *         "coll1": <hash>,
     *         ...
     *     },
     *     "db1": {
     *         "coll0": <hash>,
     *         "coll1": <hash>,
     *         ...
     *     }
     *     ...
     * }
     * @property {Array.<Object>} [entriesAfterBackup] A list of oplog entries after the backup
     * cursor was opened. Used to perform a PIT restore.
     * @property {Object} [expectedStableTimestamp] The expected stable timestamp after magic
     * restore completes.
     */
    constructor({rst, pipeDir, insertHigherTermOplogEntry}) {
        this.rst = rst;
        this.pipeDir = pipeDir;

        this.backupSource = this._selectBackupSource();
        // Data files are backed up from the source into 'backupDbPath'. We'll copy these data files
        // into separate dbpaths for each node, ending with 'restore_{nodeId}'.
        this.backupDbPath = pipeDir + "/backup";
        this.restoreDbPaths = [];
        this.rst.nodes.forEach((node) => {
            const restoreDbPath = pipeDir + "/restore_" + this.rst.getNodeId(node);
            this.restoreDbPaths.push(restoreDbPath);
        });

        // isPit is set when we receive the restoreConfiguration.
        this.isPit = false;
        this.insertHigherTermOplogEntry = insertHigherTermOplogEntry || false;
        // Default high term value.
        this.restoreToHigherTermThan = 100;

        // The replica set config will be the same across nodes in a cluster.
        this.expectedConfig = this.rst.getPrimary().adminCommand({replSetGetConfig: 1}).config;
        // These fields are set during the restore process.
        this.backupCursor = undefined;
        this.backupId = undefined;
        this.checkpointTimestamp = undefined;
        this.pointInTimeTimestamp = undefined;
        // When we perform a selective restore, we need to store a list of collections to restore to
        // pass into the restore configuration. This list includes namespaces and UUIDs for this
        // particular replica set.
        this.collectionsToRestore = [];

        // Store dbhashes for each db and collection prior to restore. After magic restore
        // completes, we compare the post-restore hashes with this object to check for consistency.
        this.preRestoreDbHashes = {};
        // Store the oplog entries after we open the backup cursor, as we need to pass these entries
        // into a PIT restore.
        this.entriesAfterBackup = [];
        // Store the expected stable timestamp so we can check a replica set after a restore
        // completes. For replica set nodes, this will be either the checkpointTimestamp or
        // pointInTimeTimestamp, depending on if the restore is PIT or not. For a PIT sharded
        // cluster restore, the expected stable timestamp may differ from the cluster-wide
        // point-in-time timestamp. This is because while each shard is consistent up to this point,
        // the stable timestamp is set to the last entry in the oplog. For certain shards, their
        // last oplog entry might be before the cluster-wide point-in-time timestamp.
        this.expectedStableTimestamp = undefined;
    }

    /**
     * Helper function that selects the node to use for data files. For single-node sets we'll use
     * the primary, but for multi-node sets we'll use the first secondary. In production, we often
     * retrieve the backup from a secondary node to reduce load on the primary.
     */
    _selectBackupSource() {
        let backupSource;
        if (this.rst.nodes.length === 1) {
            backupSource = this.rst.getPrimary();
            jsTestLog(`Selecting primary ${backupSource.host} as backup source.`);
            return backupSource;
        }
        backupSource = this.rst.getSecondary();
        jsTestLog(`Selecting secondary ${backupSource.host} as backup source.`);
        return backupSource;
    }

    /**
     * Helper function that returns the checkpoint timestamp from the backup cursor. Used in tests
     * that need this timestamp to make assertions about data before and after the backup time.
     */
    getCheckpointTimestamp() {
        return this.checkpointTimestamp;
    }

    /**
     * Helper function that returns the dbpath for the backup. Used to start a regular node after
     * magic restore completes. Parameterizes the dbpath to allow for multi-node clusters.
     */
    getBackupDbPath() {
        return MagicRestoreTest.parameterizeDbpath(this.restoreDbPaths[0]);
    }

    /**
     * Helper function that returns the expected config after the restore.
     */
    getExpectedConfig() {
        return this.expectedConfig;
    }

    /**
     * Helper function that returns the collections to restore during magic restore.
     */
    getCollectionsToRestore() {
        return this.collectionsToRestore;
    }

    /**
     * Takes a checkpoint and opens the backup cursor on the source. backupCursorOpts is an optional
     * parameter which will be passed to the openBackupCursor call if provided. This function
     * returns the backup cursor metadata object.
     */
    takeCheckpointAndOpenBackup(backupCursorOpts = {}) {
        // If we are taking a backup of a secondary, we should ensure expected writes have been
        // replicated.
        this.rst.awaitReplication();
        // Take the initial checkpoint.
        assert.commandWorked(this.backupSource.adminCommand({fsync: 1}));

        // Open a backup cursor on the checkpoint.
        this.backupCursor =
            backupUtils.openBackupCursor(this.backupSource.getDB("admin"), backupCursorOpts);
        // Print the backup metadata document.
        assert(this.backupCursor.hasNext());
        const {metadata} = this.backupCursor.next();
        jsTestLog("Backup cursor metadata document: " + tojson(metadata));
        this.backupId = metadata.backupId;
        this.checkpointTimestamp = metadata.checkpointTimestamp;
        return metadata;
    }

    /**
     * Copies data files from the source dbpath to the backup dbpath. 'collectionsToRestore' is used
     * to test selective restore.
     */
    copyFiles(collectionsToRestore = []) {
        resetDbpath(this.backupDbPath);
        // TODO(SERVER-13455): Replace `journal/` with the configurable journal path.
        mkdir(this.backupDbPath + "/journal");
        while (this.backupCursor.hasNext()) {
            const doc = this.backupCursor.next();
            // If 'collectionsToRestore' parameter has non-zero elements, this indicates we are
            // testing selective restore. If we are not testing selective restore, we should copy
            // all files.
            const isSelectiveRestore = collectionsToRestore.length > 0;
            const shouldCopy =
                !isSelectiveRestore || collectionsToRestore.includes(doc.ns) || doc.required;
            if (shouldCopy) {
                jsTestLog("Copying for backup: " + tojson(doc));
                backupUtils.copyFileHelper({filename: doc.filename, fileSize: doc.fileSize},
                                           this.backupSource.dbpath,
                                           this.backupDbPath);
                if (isSelectiveRestore && doc.ns && doc.uuid) {
                    const exists = this.collectionsToRestore.some(
                        (item) => item.ns === doc.ns &&
                            item.uuid.toString() === UUID(doc.uuid).toString());
                    if (!exists) {
                        this.collectionsToRestore.push({ns: doc.ns, uuid: UUID(doc.uuid)});
                    }
                }
            } else {
                jsTestLog("Skipping backup for: " + tojson(doc));
            }
        }
    }

    /**
     * Copies data files from the source dbpath to the backup dbpath, and then closes the backup
     * cursor. Copies the data files from the backup path to each node's restore db path.
     */
    copyFilesAndCloseBackup(collectionsToRestore = []) {
        this.copyFiles(collectionsToRestore);
        this.backupCursor.close();
        this.restoreDbPaths.forEach((restoreDbPath) => {
            resetDbpath(restoreDbPath);
            MagicRestoreTest.copyBackupFilesToDir(this.backupDbPath, restoreDbPath);
        });
    }

    /**
     * Extends the backup cursor, copies the extend files and closes the backup cursor.
     */
    extendAndCloseBackup(node, maxCheckpointTs, collectionsToRestore) {
        backupUtils.extendBackupCursor(node, this.backupId, maxCheckpointTs);
        backupUtils.copyBackupCursorExtendFiles(this.backupCursor,
                                                collectionsToRestore,
                                                this.backupSource.dbpath,
                                                this.backupDbPath,
                                                false /*async*/);
        this.backupCursor.close();
        this.restoreDbPaths.forEach((restoreDbPath) => {
            resetDbpath(restoreDbPath);
            MagicRestoreTest.copyBackupFilesToDir(this.backupDbPath, restoreDbPath);
        });
    }

    // Copies backup cursor data files from directory to another. Makes the destination directory if
    // needed. Used to copy one set of backup files to multiple nodes.
    static copyBackupFilesToDir(source, dest) {
        if (!fileExists(dest)) {
            assert(mkdir(dest).created);
        }
        jsTestLog(`Copying data files from source path ${source} to destination path ${dest}`);
        copyDir(source, dest);
    }

    /**
     * Helper function that generates the magic restore named pipe path for testing. 'pipeDir'
     * is the directory in the filesystem in which we create the named pipe.
     */
    static _generateMagicRestorePipePath(pipeDir) {
        const pipeName = "magic_restore_named_pipe";
        const pipePath = `${pipeDir}/tmp/${pipeName}`;
        if (!fileExists(pipeDir + "/tmp/")) {
            assert(mkdir(pipeDir + "/tmp/").created);
        }
        return {pipeName, pipePath};
    }

    /**
     * Helper function that writes an array of JavaScript objects into a named pipe. 'objs' will be
     * serialized into BSON and written into the named pipe path generated by
     * '_generateMagicRestorePipePath'. The function is static as it is used in passthrough testing
     * as well.
     */
    static writeObjsToMagicRestorePipe(pipeDir, objs, persistPipe = false) {
        const {pipeName, pipePath} = MagicRestoreTest._generateMagicRestorePipePath(pipeDir);
        _writeTestPipeObjects(pipeName, objs.length, objs, pipeDir + "/tmp/", persistPipe);
        // Creating the named pipe is async, so we should wait until the file exists.
        assert.soon(() => fileExists(pipePath));
    }

    /**
     * Helper function that starts a magic restore node on the 'backupDbPath'. Waits for the process
     * to exit cleanly. The function is static as it is used in passthrough testing as well.
     */
    static runMagicRestoreNode(pipeDir, backupDbPath, options = {}) {
        const {pipePath} = MagicRestoreTest._generateMagicRestorePipePath(pipeDir);
        // Magic restore will exit the mongod process cleanly. 'runMongod' may acquire a connection
        // to mongod before it exits, and so we wait for the process to exit in the 'assert.soon'
        // below. If mongod exits before we acquire a connection, 'conn' will be null. In this case,
        // if mongod exits with non-zero exit code, the runner will throw a StopError.
        const conn = MongoRunner.runMongod({
            dbpath: backupDbPath,
            noCleanData: true,
            magicRestore: "",
            env: {namedPipeInput: pipePath},
            ...options
        });
        if (conn) {
            assert.soon(() => {
                const res = checkProgram(conn.pid);
                return !res.alive && res.exitCode == MongoRunner.EXIT_CLEAN;
            }, "Expected magic restore to exit mongod cleanly");
        }
    }

    /**
     * Avoids nested loops (some including the config servers and some without) in the test itself.
     */
    static getAllNodes(numShards, numNodes) {
        const result = [];
        for (let rsIndex = 0; rsIndex < numShards; rsIndex++) {
            for (let nodeIndex = 0; nodeIndex < numNodes; nodeIndex++) {
                result.push([rsIndex, nodeIndex]);
            }
        }
        return result;
    }

    /**
     * Helper function that retrieves collection dbhashes for each database on the given node.
     * Produces an object with the shape:
     * {
     *     "db0": {
     *         "coll0": <hash>,
     *         "coll1": <hash>,
     *         ...
     *     },
     *     "db1": {
     *         "coll0": <hash>,
     *         "coll1": <hash>,
     *         ...
     *     }
     *     ...
     * }
     *
     */
    _getDbHashes(node) {
        const dbHashes = {};
        const listDbs =
            assert.commandWorked(node.adminCommand({"listDatabases": 1, "nameOnly": true}));
        for (let db of listDbs["databases"]) {
            const dbName = db["name"];
            const dbHashRes = node.getDB(dbName).runCommand({dbHash: 1});
            dbHashes[dbName] = dbHashRes.collections;
        }
        return dbHashes;
    }

    /**
     * Calculates and stores pre-restore collection hashes.
     */
    storePreRestoreDbHashes() {
        this.preRestoreDbHashes = this._getDbHashes(this.backupSource);
    }

    /**
     * Retrieves collection hashes after the restore and compares them against pre-restore hashes
     * for consistency checking.
     */
    checkPostRestoreDbHashes(excludedCollections) {
        this.rst.nodes.forEach((node) => {
            const pre = this.preRestoreDbHashes;
            const post = this._getDbHashes(node);
            // Check that all databases and collections hashed before the restore are the same on
            // the restored node.
            for (const dbName in pre) {
                jsTestLog(`Checking dbhashes for ${dbName} db`);
                assert(post.hasOwnProperty(dbName),
                       `Restored node is missing db ${dbName} in post-restore hashes`);
                const preDb = pre[dbName];
                const postDb = post[dbName];
                for (let collName in preDb) {
                    jsTestLog(`Checking dbhashes for ns ${dbName}.${collName}`);
                    if (excludedCollections.includes(collName)) {
                        continue;
                    }
                    assert(postDb.hasOwnProperty(collName),
                           `Restored node is missing dbhash for ${dbName}.${
                               collName} ns in post-restore hashes`);
                    assert.eq(
                        preDb[collName],
                        postDb[collName],
                        `Dbhash values are not equal for ${dbName}.${collName}. pre-restore hash: ${
                            preDb[collName]}, post-restore hash: ${postDb[collName]}`);
                }
            }

            jsTestLog(`Checking post-restore dbhashes for extra databases or collections`);
            // Check that the restored node does not have extra databases or collections.
            for (const dbName in post) {
                assert(pre.hasOwnProperty(dbName),
                       `Restored node has extra db ${dbName} in post-restore hashes`);
                const preDb = pre[dbName];
                const postDb = post[dbName];

                for (let collName in postDb) {
                    if (excludedCollections.includes(collName)) {
                        continue;
                    }
                    assert(preDb.hasOwnProperty(collName),
                           `Restored node has extra collection ${dbName}.${
                               collName} in post-restore hashes`);
                }
            }
        });
    }

    /**
     * Replaces the trailing "_0" from the backupPath of the first node of a replica set by "_$node"
     * so startSet uses the correct dbpath for each node in the replica set.
     */
    static parameterizeDbpath(backupPath) {
        return backupPath.slice(0, -1) + "$node";
    }

    /**
     * Retrieves all oplog entries that occurred after the checkpoint timestamp on the source node.
     * Returns an object with the timestamp of the last oplog entry, as well as the oplog
     * entry array
     */
    getEntriesAfterBackup(sourceNode = this.backupSource) {
        let oplog = sourceNode.getDB("local").getCollection('oplog.rs');
        const entriesAfterBackup =
            oplog.find({ts: {$gt: this.checkpointTimestamp}}).sort({ts: 1}).toArray();
        // We expect the caller to insert data after the backup, for PIT restores.
        assert(entriesAfterBackup.length > 0);
        this.entriesAfterBackup = entriesAfterBackup;
        return {
            lastOplogEntryTs: entriesAfterBackup[entriesAfterBackup.length - 1].ts,
            entriesAfterBackup
        };
    }

    /**
     * A helper function that makes multiple assertions on the restore node. Helpful to avoid
     * needing to make each individual assertion in each test.
     */
    postRestoreChecks({
        node,
        dbName,
        collName,
        expectedOplogCountForNs,
        opFilter,
        expectedNumDocsSnapshot,
        rolesCollUuid,
        userCollUuid,
        logPath,
    }) {
        node.setSecondaryOk();
        const restoredConfig =
            assert.commandWorked(node.adminCommand({replSetGetConfig: 1})).config;
        this._assertConfigIsCorrect(this.expectedConfig, restoredConfig);
        this.assertOplogCountForNamespace(
            node, {ns: dbName + "." + collName, op: opFilter}, expectedOplogCountForNs);
        this._assertMinValidIsCorrect(node);
        this._assertStableCheckpointIsCorrectAfterRestore(node);
        this._assertCannotDoSnapshotRead(
            node, expectedNumDocsSnapshot /* expectedNumDocsSnapshot */, dbName, collName);
        if (rolesCollUuid && userCollUuid) {
            assert.eq(rolesCollUuid, this.getCollUuid(node, "admin", "system.roles"));
            assert.eq(userCollUuid, this.getCollUuid(node, "admin", "system.users"));
        }
        if (logPath) {
            this._checkRestoreSpecificLogs(logPath);
        }
    }

    /**
     * Performs a find on the oplog for the given name space and asserts that the expected number of
     * entries exists. Optionally takes an op type to filter.
     */
    assertOplogCountForNamespace(node, findObj, expectedNumEntries) {
        const entries =
            node.getDB("local").getCollection('oplog.rs').find(findObj).sort({ts: -1}).toArray();
        assert.eq(entries.length,
                  expectedNumEntries,
                  `Expected ${expectedNumEntries} entries but found ${entries.length}. entries = ${
                      tojson(entries)}`);
    }

    /**
     * Adds the 'restoreToHigherTermThan' field to the restore configuration if this instance is
     * testing the higher term no-op behavior.
     */
    appendRestoreToHigherTermThanIfNeeded(restoreConfiguration) {
        if (this.insertHigherTermOplogEntry) {
            restoreConfiguration.restoreToHigherTermThan = NumberLong(this.restoreToHigherTermThan);
        }
        return restoreConfiguration;
    }

    /**
     * Combines writing objects to the named pipe and running magic restore.
     */
    writeObjsAndRunMagicRestore(restoreConfiguration, entriesAfterBackup, options) {
        this.pointInTimeTimestamp = restoreConfiguration.pointInTimeTimestamp;
        if (!this.expectedStableTimestamp) {
            this.expectedStableTimestamp = this.pointInTimeTimestamp;
        }
        if (this.pointInTimeTimestamp) {
            assert(entriesAfterBackup.length > 0);
            this.isPit = true;
        }
        jsTestLog("Restore configuration: " + tojson(restoreConfiguration));

        // Restore each node in serial.
        this.rst.nodes.forEach((node, idx) => {
            jsTestLog(`Restoring node ${node.host}`);
            MagicRestoreTest.writeObjsToMagicRestorePipe(
                this.pipeDir, [restoreConfiguration, ...entriesAfterBackup]);
            MagicRestoreTest.runMagicRestoreNode(this.pipeDir, this.restoreDbPaths[idx], options);
        });
    }

    /**
     * Asserts that two config arguments are equal. If we are testing higher term behavior in the
     * test, modifies the expected term on the source config.
     */
    _assertConfigIsCorrect(srcConfig, dstConfig) {
        // If we passed in a value for the 'restoreToHigherTermThan' field in the restore config, a
        // no-op oplog entry was inserted in the oplog with that term value + 100. On startup, the
        // replica set node sets its term to this value. A new election occurred when the replica
        // set restarted, so we must also increment the term by 1 regardless of if we passed in a
        // higher term value.
        const expectedTerm = this.insertHigherTermOplogEntry ? this.restoreToHigherTermThan + 101
                                                             : srcConfig.term + 1;
        // Make a copy to not modify the object passed by the caller.
        srcConfig = Object.assign({}, srcConfig);
        srcConfig.term = expectedTerm;
        assert.eq(srcConfig, dstConfig);
    }

    /**
     * Asserts that the stable checkpoint timestamp on the restored node is as expected. For a
     * non-PIT restore, the timestamp should be equal to the checkpoint timestamp from the backup
     * cursor. For a PIT restore, it should be equal to the top of the oplog. For any restore that
     * inserts a no-op oplog entry with a higher term, the stable checkpoint timestamp should be
     * equal to the timestamp of that entry.
     */
    _assertStableCheckpointIsCorrectAfterRestore(restoreNode) {
        // For a PIT restore on a sharded cluster, the lastStableCheckpointTs of a given shard might
        // be behind the global pointInTimeTimestamp, as it is set to the top of the oplog for a
        // given shard. To continue with this check, we store the expected stable timestamp after
        // the restore.
        let lastStableCheckpointTs =
            this.isPit ? this.expectedStableTimestamp : this.checkpointTimestamp;

        if (this.insertHigherTermOplogEntry) {
            const oplog = restoreNode.getDB("local").getCollection('oplog.rs');
            const incrementTermEntry =
                oplog.findOne({op: "n", "o.msg": "restore incrementing term"});
            assert(incrementTermEntry);
            assert.eq(incrementTermEntry.t, this.restoreToHigherTermThan + 100);
            // If we've inserted a no-op oplog entry with a higher term during magic restore, we'll
            // have updated the stable timestamp.
            lastStableCheckpointTs = incrementTermEntry.ts;
        }

        // Ensure that the last stable checkpoint is as expected. As the timestamp is greater than
        // 0, this means the magic restore took a stable checkpoint on shutdown.
        const {lastStableRecoveryTimestamp} =
            assert.commandWorked(restoreNode.adminCommand({replSetGetStatus: 1}));
        assert(timestampCmp(lastStableRecoveryTimestamp, lastStableCheckpointTs) == 0,
               `timestampCmp(${tojson(lastStableRecoveryTimestamp)}, ${
                   tojson(lastStableCheckpointTs)}) is ${
                   timestampCmp(lastStableRecoveryTimestamp, lastStableCheckpointTs)}, not 0`);
    }

    /**
     * Assert that the minvalid document has been set correctly in magic restore.
     */
    _assertMinValidIsCorrect(restoreNode) {
        const minValid = restoreNode.getCollection('local.replset.minvalid').findOne();
        assert.eq(minValid,
                  {_id: ObjectId("000000000000000000000000"), t: -1, ts: Timestamp(0, 1)});
    }

    /**
     * Assert that a restored node cannot complete a snapshot read at a timestamp earlier than the
     * last stable checkpoint timestamp.
     */
    _assertCannotDoSnapshotRead(restoreNode, expectedNumDocsSnapshot, db = "db", coll = "coll") {
        const {lastStableRecoveryTimestamp} =
            assert.commandWorked(restoreNode.adminCommand({replSetGetStatus: 1}));
        // A restored node will not preserve any history. The oldest timestamp should be set to the
        // stable timestamp at the end of a non-PIT restore.
        let res = restoreNode.getDB(db).runCommand({
            find: coll,
            readConcern: {
                level: "snapshot",
                atClusterTime: Timestamp(lastStableRecoveryTimestamp.getTime() - 1,
                                         lastStableRecoveryTimestamp.getInc())
            }
        });
        assert.commandFailedWithCode(res, ErrorCodes.SnapshotTooOld);

        // A snapshot read at the last stable timestamp should succeed.
        res = restoreNode.getDB(db).runCommand({
            find: coll,
            readConcern: {level: "snapshot", atClusterTime: lastStableRecoveryTimestamp}
        });
        assert.commandWorked(res);

        // With large documents or a large number of documents, there might be more than one batch.
        let documents = [];
        documents = documents.concat(res.cursor.firstBatch);

        while (res.cursor.id != 0) {
            res = assert.commandWorked(
                restoreNode.getDB(db).runCommand({getMore: res.cursor.id, collection: coll}));
            documents = documents.concat(res.cursor.nextBatch);
        }

        assert.eq(documents.length, expectedNumDocsSnapshot);
    }

    /**
     * Get the UUID for a given collection.
     */
    getCollUuid(node, dbName, collName) {
        return node.getDB(dbName).getCollectionInfos({name: collName})[0].info.uuid;
    }

    /**
     * Checks the log file specified at 'logpath', and ensures that it contains logs with the
     * 'RESTORE' component. The function also ensures the first and last log lines are as expected.
     */
    _checkRestoreSpecificLogs(logpath) {
        // When splitting the logs on new lines, the last element will be an empty string, so we
        // should filter it out.
        let logs = cat(logpath)
                       .split("\n")
                       .filter(line => line.trim() !== "")
                       .map(line => JSON.parse(line))
                       .filter(json => json.c === 'RESTORE');
        assert.eq(logs[0].msg, "Beginning magic restore");
        assert.eq(logs[logs.length - 1].msg, "Finished magic restore");
    }
}
