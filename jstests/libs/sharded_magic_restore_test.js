/**
 * This class implements helpers for testing the magic restore process with a sharded cluster. It
 * wraps a ShardingTest object and maintains a MagicRestoreTest object per replica set. It handles
 * the logic to extend backup cursors as well as running restore on each replica set in the shard.
 */
import {MagicRestoreTest} from "jstests/libs/magic_restore_test.js";

export class ShardedMagicRestoreTest {
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
        // shardingRename is a list of objects.
        this.shardingRename = undefined;
        this.shardIdentityDocuments = undefined;
        this.maxCheckpointTs = undefined;
        this.pointInTimeTimestamp = undefined;
        // We combine the lists of restored collections from each shard into one
        // 'collectionsToRestore' list, and pass that into the restoreConfiguration. This is used by
        // the config server to remove metadata about unrestored collections.
        this.collectionsToRestore = [];
    }

    mkdirAndResetPath(path) {
        const {exists, created} = mkdir(path);
        exists ? resetDbpath(path) : assert(created);
    }

    takeCheckpointsAndOpenBackups() {
        this.magicRestoreTests.forEach((magicRestoreTest) => {
            magicRestoreTest.takeCheckpointAndOpenBackup();
        });
    }

    getConfigRestoreTest() {
        return this.configRestoreTest;
    }

    getShardRestoreTests() {
        return this.shardRestoreTests;
    }

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
     * server.
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
        jsTestLog(`Combined each shard's collectionsToRestore lists: ${
            tojson(this.collectionsToRestore)}`);
    }

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
                restoreConfiguration.collectionsToRestore = this.collectionsToRestore;
                rstOptions.restore = '';
            }
            restoreConfiguration =
                magicRestoreTest.appendRestoreToHigherTermThanIfNeeded(restoreConfiguration);
            magicRestoreTest.writeObjsAndRunMagicRestore(
                restoreConfiguration, magicRestoreTest.entriesAfterBackup, rstOptions);
        });
    }

    storePreRestoreDbHashes() {
        this.magicRestoreTests.forEach((magicRestoreTest) => {
            magicRestoreTest.storePreRestoreDbHashes();
        });
    }

    checkPostRestoreDbHashes(excludedCollections) {
        jsTestLog("Running dbhash checks for sharded cluster magic restore test");
        this.magicRestoreTests.forEach((magicRestoreTest) => {
            magicRestoreTest.checkPostRestoreDbHashes(excludedCollections);
        });
    }

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
