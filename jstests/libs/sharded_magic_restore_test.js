import {MagicRestoreTest} from "jstests/libs/magic_restore_test.js";

/**
 * This class implements helpers for testing the magic restore process with a sharded cluster. It
 * wraps a ShardingTest object and maintains a MagicRestoreTest object per replica set. It handles
 * the logic to extend backup cursors as well as running restore on each replica set in the shard.
 *
 * @class
 */
export class ShardedMagicRestoreTest {
    /**
     * Creates a new ShardedMagicRestoreTest instance.
     *
     * @constructor
     * @param {Object} [params] The parameters object for the ShardedMagicRestoreTest.
     * @param {Object} [params.st] The ShardingTest object.
     * @param {string} [params.pipeDir] The file path of the named pipe. The pipe is used by magic
     *     restore to read the restore configuration and any additional PIT oplog entries. This
     *     should usually be 'MongoRunner.dataDir'.
     * @param {boolean} [params.insertHigherTermOplogEntry=false] Whether to insert a higher-term
     *     oplog entry during the restore procedure. This is used by Cloud to maintain MongoDB
     *     driver connections to a node after restore.
     *
     * @property {number} [numShards] Number of shards in the cluster.
     * @property {number} [clusterId] The sharded cluster ID. Used when updating shard identity
     * document.
     * @property {Object} [configRestoreTest] The MagicRestoreTest object referencing the config
     * server. Used when config server-specific logic is required.
     * @property {Array.<Object>} [shardRestoreTests] The MagicRestoreTest objects referencing the
     * shard servers.
     * @property {Array.<Object>} [magicRestoreTests] The MagicRestoreTest objects for all replica
     * sets in the sharded clusters.
     * @property {Array.<Object>} [shardingRename] A list of objects containing old and new shard
     * IDs and the new shard connection string. Used for shard renames.
     * @property {Array.<Object>} [shardIdentityDocuments] A list of objects containing new shard
     * identity documents for each shard replica set.
     * @property {Object} [maxCheckpointTs] The maximum checkpoint timestamp amongst all replica
     * sets in the sharded cluster.
     * @property {Object} [pointInTimeTimestamp] The timestamp to restore to with additional oplog
     * entries in a PIT restore.
     * @property {Array.<Object>} [collectionsToRestore] A list of objects containing namespace and
     * UUID pairs, combined from each individual shard. Used to perform a selective restore.
     */
    constructor({st, pipeDir, insertHigherTermOplogEntry}) {
        this.st = st;
        this.pipeDir = pipeDir;
        this.insertHigherTermOplogEntry = insertHigherTermOplogEntry;
        this.numShards = this.st._rs.length;
        this.clusterId = st.s.getCollection('config.version').findOne().clusterId;

        jsTestLog("Creating MagicRestoreTest fixture for config replica set");
        let configDir = `${MongoRunner.dataDir}/config`;
        this.mkdirAndResetPath(configDir);
        this.configRestoreTest = new MagicRestoreTest({
            rst: this.st.configRS,
            pipeDir: configDir,
            insertHigherTermOplogEntry: insertHigherTermOplogEntry
        });
        this.shardRestoreTests = [];
        for (let i = 0; i < this.numShards; i++) {
            jsTestLog(`Creating MagicRestoreTest fixture for shard replica set ${i}`);
            let shardDir = `${MongoRunner.dataDir}/shard${i}`;
            this.mkdirAndResetPath(shardDir);
            const magicRestoreTest = new MagicRestoreTest({
                rst: this.st["rs" + i],
                pipeDir: shardDir,
                insertHigherTermOplogEntry: insertHigherTermOplogEntry
            });
            this.shardRestoreTests.push(magicRestoreTest);
        }
        this.magicRestoreTests = [this.configRestoreTest, ...this.shardRestoreTests];

        this.shardingRename = undefined;
        this.shardIdentityDocuments = undefined;
        this.maxCheckpointTs = undefined;
        this.pointInTimeTimestamp = undefined;
        // We combine the lists of restored collections from each shard into one
        // 'collectionsToRestore' list, and pass that into the restoreConfiguration. This is used by
        // the config server to remove metadata about unrestored collections. Used for selective
        // restore.
        this.collectionsToRestore = [];
        this.balancerSettings = undefined;
    }

    /**
     * Resets the given dbpath if it exists, or creates if it doesn't exist.
     */
    mkdirAndResetPath(path) {
        const {exists, created} = mkdir(path);
        exists ? resetDbpath(path) : assert(created);
    }

    /**
     * Takes a checkpoint and opens the backup cursor on each replica set in the sharded cluster
     */
    takeCheckpointsAndOpenBackups() {
        this.magicRestoreTests.forEach((magicRestoreTest) => {
            magicRestoreTest.takeCheckpointAndOpenBackup();
        });
    }

    /**
     * Returns the MagicRestoreTest instance managing the config server replica set.
     */
    getConfigRestoreTest() {
        return this.configRestoreTest;
    }

    /**
     * Returns a list of MagicRestoreTest instances managing the shard server replica sets.
     */
    getShardRestoreTests() {
        return this.shardRestoreTests;
    }

    /**
     * Calculates the maximum checkpoint timestamp across all replica sets in the sharded cluster.
     * Extends backup cursors on each replica set to this timestamp and copies over new data files.
     */
    findMaxCheckpointTsAndExtendBackupCursors(collectionsToRestore = []) {
        // Helper function to retrieve and set the max checkpoint timestamp.
        const updateMaxCheckpointTs = (magicRestoreTest) => {
            const ts = magicRestoreTest.getCheckpointTimestamp();
            if (timestampCmp(ts, this.maxCheckpointTs) > 0) {
                this.maxCheckpointTs = ts;
            }
        };
        this.maxCheckpointTs = Timestamp(0, 0);
        // Config servers are always restored from a full backup, even when testing selective
        // restore with 'collectionsToRestore'.
        this.configRestoreTest.copyFiles();
        updateMaxCheckpointTs(this.configRestoreTest);

        this.shardRestoreTests.forEach((magicRestoreTest) => {
            magicRestoreTest.copyFiles(collectionsToRestore);
            updateMaxCheckpointTs(magicRestoreTest);
        });

        jsTestLog("Computed maxCheckpointTs: " + tojson(this.maxCheckpointTs));

        jsTestLog("Extending backup cursors");
        this.configRestoreTest.extendAndCloseBackup(
            this.configRestoreTest.backupSource, this.maxCheckpointTs, []);
        this.shardRestoreTests.forEach((magicRestoreTest) => {
            magicRestoreTest.extendAndCloseBackup(
                magicRestoreTest.backupSource, this.maxCheckpointTs, collectionsToRestore);
        });
        // If we performed a selective backup, we should set the list of collections to restore
        // after copying files. This will be passed into the restore configuration.
        this.setCollectionsToRestore();
    }

    /**
     * Sets the point-in-time timestamp for the sharded cluster restore. Uses the latest oplog entry
     * timestamp across all replica sets in the sharded cluster.
     */
    setPointInTimeTimestamp() {
        this.pointInTimeTimestamp = Timestamp(0, 0);
        this.magicRestoreTests.forEach((magicRestoreTest) => {
            let {lastOplogEntryTs} = magicRestoreTest.getEntriesAfterBackup();
            // Store the last oplog entry for each replica set. This is used to check the stable
            // timestamp at the end of restore.
            magicRestoreTest.expectedStableTimestamp = lastOplogEntryTs;
            if (timestampCmp(lastOplogEntryTs, this.pointInTimeTimestamp) > 0) {
                this.pointInTimeTimestamp = lastOplogEntryTs;
            }
        });
        jsTestLog(
            `Computed point-in-time timestamp ${tojson(this.pointInTimeTimestamp)} for cluster`);
    }

    /**
     * Retrieves the 'collectionsToRestore' list from each shard, and combines them into one list.
     * This list of restored namespaces is passed into magic restore via the restoreConfiguration.
     * Note that we only combine the lists from shard nodes, as we take a full backup of the config
     * server. This function should only ever be called for selective magic restores.
     */
    setCollectionsToRestore() {
        this.shardRestoreTests.forEach((shardRestoreTest) => {
            shardRestoreTest.collectionsToRestore.forEach((collToRestore) => {
                const exists = this.collectionsToRestore.some(
                    (item) => item.ns === collToRestore.ns &&
                        item.uuid.toString() === collToRestore.uuid.toString());

                if (!exists) {
                    this.collectionsToRestore.push(collToRestore);
                }
            });
        });
        jsTestLog(`Combined each shard's collectionsToRestore lists for selective magic restore: ${
            tojson(this.collectionsToRestore)}`);
    }

    /**
     * Sets the balancer settings field that is passed into restore via the restore configuration.
     * The balancer settings specify whether the balancer should be enabled or disabled after
     * restore completes.
     */
    setBalancerSettings(stopped) {
        this.balancerSettings = {stopped: stopped};
    }

    /**
     * Runs magic restore on each replica set in the sharded cluster. Appends fields to the
     * 'restoreConfiguration' as needed.
     */
    runMagicRestore() {
        this.magicRestoreTests.forEach((magicRestoreTest, idx) => {
            const rstOptions = {"replSet": magicRestoreTest.rst.name};

            let restoreConfiguration = {
                "nodeType": idx > 0 ? "shard" : "configServer",
                "replicaSetConfig": magicRestoreTest.getExpectedConfig(),
                "maxCheckpointTs": this.maxCheckpointTs
            };

            if (this.pointInTimeTimestamp) {
                restoreConfiguration.pointInTimeTimestamp = this.pointInTimeTimestamp;
            }
            if (this.shardingRename) {
                restoreConfiguration.shardingRename = this.shardingRename;
            }
            if (this.shardIdentityDocuments) {
                restoreConfiguration.shardIdentityDocument = this.shardIdentityDocuments[idx];
            }
            // When we perform a selective restore, we need to store a list of collections to
            // restore to pass into the restore configuration. This list is used by the config
            // server, but we can pass it in via the restoreConfiguration for all nodes.
            if (this.collectionsToRestore.length != 0) {
                jsTestLog(
                    "Passing in --restore to mongod invocation and setting 'collectionsToRestore' on restore configuration for selective magic restore");
                restoreConfiguration.collectionsToRestore = this.collectionsToRestore;
                rstOptions.restore = '';
            }
            if (this.balancerSettings) {
                restoreConfiguration.balancerSettings = this.balancerSettings;
            }
            restoreConfiguration =
                magicRestoreTest.appendRestoreToHigherTermThanIfNeeded(restoreConfiguration);
            magicRestoreTest.writeObjsAndRunMagicRestore(
                restoreConfiguration, magicRestoreTest.entriesAfterBackup, rstOptions);
        });
    }

    /**
     * Stores pre-restore dbhashes for each replica set in the sharded cluster.
     */
    storePreRestoreDbHashes() {
        this.magicRestoreTests.forEach((magicRestoreTest) => {
            magicRestoreTest.storePreRestoreDbHashes();
        });
    }

    /**
     * Checks the pre-restore hashes against post-restore hashes on each replica set in the sharded
     * cluster.
     */
    checkPostRestoreDbHashes(excludedCollections) {
        jsTestLog("Running dbhash checks for sharded cluster magic restore test");
        this.magicRestoreTests.forEach((magicRestoreTest) => {
            magicRestoreTest.checkPostRestoreDbHashes(excludedCollections);
        });
    }

    /**
     * Sets up the sharding renames list and shard identity documents for the sharded cluster.
     * For each shard, it renames the shard (replacing `-rs` with `-dst-rs` in the shard's name and
     * connection string) and generates a new shard identity document with the renamed shard.
     */
    setUpShardingRenamesAndIdentityDocs() {
        jsTestLog("Generating sharding renames list");
        this.shardIdentityDocuments = [];
        this.shardingRename = [];
        this.shardIdentityDocuments.push({
            clusterId: this.clusterId,
            shardName: "config",
            configsvrConnectionString: this.st.configRS.getURL()
        });

        for (let i = 0; i < this.numShards; i++) {
            const shard = this.st["shard" + i];
            this.shardingRename.push({
                sourceShardName: shard.shardName,
                destinationShardName: shard.shardName.replace("-rs", "-dst-rs"),
                destinationShardConnectionString: shard.host.replace("-rs", "-dst-rs")
            });

            this.shardIdentityDocuments.push({
                clusterId: this.clusterId,
                shardName: this.shardingRename[i].destinationShardName,
                configsvrConnectionString: this.st.configRS.getURL()
            });
        }
    }
}
