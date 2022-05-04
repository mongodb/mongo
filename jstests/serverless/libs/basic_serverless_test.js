load("jstests/replsets/rslib.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");

const runCommitShardSplitAsync = function(
    rstArgs, migrationIdString, tenantIds, recipientTagName, recipientSetName) {
    load("jstests/replsets/rslib.js");

    const donorRst = createRst(rstArgs, true);
    const admin = donorRst.getPrimary().getDB("admin");

    return admin.runCommand({
        commitShardSplit: 1,
        migrationId: UUID(migrationIdString),
        tenantIds,
        recipientTagName,
        recipientSetName
    });
};

const runShardSplitCommand = function(replicaSet, cmdObj, retryOnRetryableErrors) {
    const primary = replicaSet.getPrimary();

    let res;
    assert.soon(() => {
        try {
            res = primary.adminCommand(cmdObj);

            if (!res.ok) {
                // If retry is enabled and the command failed with a NotPrimary error, continue
                // looping.
                const cmdName = Object.keys(cmdObj)[0];
                if (retryOnRetryableErrors && isRetryableError(res.code)) {
                    jsTestLog(`runShardSplitCommand retryable error. Command: ${
                        tojson(cmdObj)}, reply: ${tojson(res)}`);
                    primary = replicaSet.getPrimary();
                    return false;
                }
                jsTestLog(`runShardSplitCommand fatal error. Command: ${tojson(cmdObj)}, reply: ${
                    tojson(res)}`);
                return true;
            }
            return true;
        } catch (e) {
            if (retryOnRetryableErrors && isRetryableError(e)) {
                jsTestLog(`runShardSplitCommand retryable error. Command: ${
                    tojson(cmdObj)}, reply: ${tojson(res)}`);
                return false;
            }
            jsTestLog(`runShardSplitCommand fatal error. Command: ${tojson(cmdObj)}, reply: ${
                tojson(res)}`);
            throw e;
        }
    });
    return res;
};

/**
 * Utility class to run shard split operations.
 */
class ShardSplitOperation {
    constructor(donorSet, recipientSetName, recipientTagName, tenantIds, migrationId) {
        this.donorSet = donorSet;
        this.recipientTagName = recipientTagName;
        this.recipientSetName = recipientSetName;
        this.tenantIds = tenantIds;
        this.migrationId = migrationId;
    }

    /**
     * Starts a shard split synchronously.
     */
    commit({retryOnRetryableErrors} = {retryOnRetryableErrors: true}) {
        jsTestLog("Running commit command");

        const primary = this.donorSet.getPrimary();

        const localCmdObj = {
            commitShardSplit: 1,
            migrationId: this.migrationId,
            tenantIds: this.tenantIds,
            recipientTagName: this.recipientTagName,
            recipientSetName: this.recipientSetName
        };

        return runShardSplitCommand(this.donorSet, localCmdObj, retryOnRetryableErrors);
    }

    /**
     * Starts a shard split asynchronously and returns the Thread that runs it.
     */
    commitAsync() {
        jsTestLog("Running commitAsync command");

        const donorRst = createRstArgs(this.donorSet);
        const migrationIdString = extractUUIDFromObject(this.migrationId);

        const thread = new Thread(runCommitShardSplitAsync,
                                  donorRst,
                                  migrationIdString,
                                  this.tenantIds,
                                  this.recipientTagName,
                                  this.recipientSetName);
        thread.start();

        return thread;
    }

    /**
     * Forgets a shard split synchronously.
     */
    forget() {
        jsTestLog("Running forgetShardSplit command");

        const cmdObj = {forgetShardSplit: 1, migrationId: this.migrationId};

        assert.commandWorked(
            runShardSplitCommand(this.donorSet, cmdObj, true /* retryableOnErrors */));
    }

    /**
     * Aborts a shard split synchronously.
     */
    abort() {
        jsTestLog("Running abort command");

        const admin = this.donorSet.getPrimary().getDB("admin");

        return admin.runCommand({abortShardSplit: 1, migrationId: this.migrationId});
    }
}

/**
 * Utility class to create a ReplicaSetTest that provides functionnality to run a shard split
 * operation.
 */
class BasicServerlessTest {
    constructor({recipientTagName, recipientSetName, quickGarbageCollection = false, nodeOptions}) {
        nodeOptions = nodeOptions || {};
        if (quickGarbageCollection) {
            nodeOptions["setParameter"] = nodeOptions["setParameter"] || {};
            Object.assign(nodeOptions["setParameter"],
                          {shardSplitGarbageCollectionDelayMS: 1000, ttlMonitorSleepSecs: 1});
        }
        this.donor = new ReplSetTest({name: "donor", nodes: 3, serverless: true, nodeOptions});
        this.donor.startSet();
        this.donor.initiate();

        this.recipientTagName = recipientTagName;
        this.recipientSetName = recipientSetName;
        this.recipientNodes = [];
    }

    /*
     * Removes and stops the recipient nodes and then stops the donor nodes.
     * @param {shouldRestart} indicates whether stop() is being called with the intent to call
     * start() with restart=true for the same node(s) n.
     */
    stop({shouldRestart = false} = {}) {
        this.removeAndStopRecipientNodes();
        // If we validate, it will try to list all collections and the migrated collections will
        // return a TenantMigrationCommitted error.
        this.donor.stopSet(
            undefined /* signal */, shouldRestart /* forRestart */, {skipValidation: 1});
    }

    /*
     * Returns a ShardSplitOperation object to run a shard split.
     * @param {tenantIds} tells which tenant ids to run a split for.
     */
    createSplitOperation(tenantIds) {
        const migrationId = UUID();
        jsTestLog("Asserting no state document exist before command");
        assert.isnull(findMigration(this.donor.getPrimary(), migrationId));

        return new ShardSplitOperation(
            this.donor, this.recipientSetName, this.recipientTagName, tenantIds, migrationId);
    }

    /*
     * Add recipient nodes to the current donor set.
     * @param {numNodes} indicates the number of recipient nodes to be added.
     */
    addRecipientNodes(numNodes) {
        numNodes = numNodes || 3;  // default to three nodes

        if (this.recipientNodes.lengh > 0) {
            throw new Error("Recipient nodes may only be added once");
        }

        jsTestLog(`Adding ${numNodes} non-voting recipient nodes to donor`);
        const donor = this.donor;
        for (let i = 0; i < numNodes; ++i) {
            this.recipientNodes.push(donor.add());
        }

        const primary = donor.getPrimary();
        const admin = primary.getDB('admin');
        const config = donor.getReplSetConfigFromNode();
        config.version++;

        // ensure recipient nodes are added as non-voting members
        this.recipientNodes.forEach(node => {
            config.members.push({
                host: node.host,
                votes: 0,
                priority: 0,
                tags: {[this.recipientTagName]: ObjectId().valueOf()}
            });
        });

        // reindex all members from 0
        config.members = config.members.map((member, idx) => {
            member._id = idx;
            return member;
        });

        assert.commandWorked(admin.runCommand({replSetReconfig: config}));
        this.recipientNodes.forEach(node => donor.waitForState(node, ReplSetTest.State.SECONDARY));
    }

    /*
     * Remove and stops the recipient nodes from the donor set.
     */
    removeAndStopRecipientNodes() {
        print("Removing and stopping recipient nodes");
        const recipientNodes = this.recipientNodes.splice(0);
        recipientNodes.forEach(node => {
            if (this.donor.nodes.includes(node)) {
                this.donor.remove(node);
            } else {
                MongoRunner.stopMongod(node);
            }
        });
    }

    /**
     * Crafts a tenant database name.
     */
    tenantDB(tenantId, dbName) {
        return `${tenantId}_${dbName}`;
    }

    /*
     * Lookup and return the tenant migration access blocker on a node for the given tenant.
     * @param {donorNode} donor node on which the request will be sent.
     * @param {tenantId} tenant id to lookup for tenant access blockers.
     */
    getTenantMigrationAccessBlocker({node, tenantId}) {
        const res = assert.commandWorked(node.adminCommand({serverStatus: 1}));
        const tenantMigrationAccessBlocker = res.tenantMigrationAccessBlocker;
        if (!tenantMigrationAccessBlocker) {
            return undefined;
        }

        tenantMigrationAccessBlocker.donor =
            tenantMigrationAccessBlocker[tenantId] && tenantMigrationAccessBlocker[tenantId].donor;
        return tenantMigrationAccessBlocker;
    }

    /*
     * Takes an rstArgs produced by createArgs and remove the recipient nodes from it.
     */
    removeRecipientsFromRstArgs(rstArgs) {
        rstArgs.nodeHosts = rstArgs.nodeHosts.filter(nodeString => {
            const port = parseInt(nodeString.split(":")[1]);
            return !this.recipientNodes.some(node => node.port == port);
        });
    }

    /*
     *  Wait for state document garbage collection by polling for when the document has been removed
     * from the tenantSplitDonors namespace, and all access blockers have been removed.
     * @param {migrationId} id that was used for the commitShardSplit command.
     * @param {tenantIds} tenant ids of the shard split.
     */
    waitForGarbageCollection(migrationId, tenantIds) {
        jsTestLog("Wait for garbage collection");
        const donorNodes = this.donor.nodes;
        assert.soon(() => donorNodes.every(node => {
            const donorDocumentDeleted =
                node.getCollection(BasicServerlessTest.kConfigSplitDonorsNS).count({
                    _id: migrationId
                }) === 0;
            const allAccessBlockersRemoved =
                tenantIds.every(id => this.getTenantMigrationAccessBlocker({node, id}) == null);

            const result = donorDocumentDeleted && allAccessBlockersRemoved;
            if (!result) {
                const status = [];
                if (!donorDocumentDeleted) {
                    status.push(`donor document to be deleted (docCount=${
                        node.getCollection(BasicServerlessTest.kConfigSplitDonorsNS).count({
                            _id: migrationId
                        })})`);
                }

                if (!allAccessBlockersRemoved) {
                    const tenantsWithBlockers = tenantIds.filter(
                        id => this.getTenantMigrationAccessBlocker({node, id}) != null);
                    status.push(`access blockers to be removed (${tenantsWithBlockers})`);
                }
            }
            return donorDocumentDeleted && allAccessBlockersRemoved;
        }));
    }

    removeRecipientNodesFromDonor() {
        jsTestLog("Removing recipient nodes from the donor.");
        this.donor.nodes = this.donor.nodes.filter(node => !this.recipientNodes.includes(node));
        this.donor.ports =
            this.donor.ports.filter(port => !this.recipientNodes.some(node => node.port === port));
    }

    /*
     * Look up tenant access blockers for the given tenant ids and will check, based upon the
     * expected state the access blockers are expected to be, that the different fields are
     * properly set such as `blockTimestamp`, `abortOpTime` or `commitOpTime`.
     * @param {migrationId} the current shard split id.
     * @param {tenantIds} tenant ids of the shard split.
     * @param {expectedState} expected state the tenant access blocker to be in.
     */
    validateTenantAccessBlockers(migrationId, tenantIds, expectedState) {
        let donorPrimary = this.donor.getPrimary();
        const stateDoc = findMigration(donorPrimary, migrationId);
        assert.soon(() => tenantIds.every(tenantId => {
            const donorMtab =
                this.getTenantMigrationAccessBlocker({node: donorPrimary, tenantId}).donor;
            const tenantAccessBlockersBlockRW = donorMtab.state == expectedState;

            const tenantAccessBlockersBlockTimestamp =
                bsonWoCompare(donorMtab.blockTimestamp, stateDoc.blockTimestamp) == 0;

            let tenantAccessBlockersAbortTimestamp = true;
            if (donorMtab.state > TenantMigrationTest.DonorAccessState.kBlockWritesAndReads) {
                let donorAbortOrCommitOpTime =
                    donorMtab.state == TenantMigrationTest.DonorAccessState.kAborted
                    ? donorMtab.abortOpTime
                    : donorMtab.commitOpTime;
                tenantAccessBlockersAbortTimestamp =
                    bsonWoCompare(donorAbortOrCommitOpTime, stateDoc.commitOrAbortOpTime) == 0;
            }
            return tenantAccessBlockersBlockRW && tenantAccessBlockersBlockTimestamp &&
                tenantAccessBlockersAbortTimestamp;
        }));
    }
}

BasicServerlessTest.kConfigSplitDonorsNS = "config.tenantSplitDonors";

function findMigration(primary, migrationId) {
    const donorsCollection = primary.getCollection(BasicServerlessTest.kConfigSplitDonorsNS);
    return donorsCollection.findOne({"_id": migrationId});
}

function cleanupMigrationDocument(primary, migrationId) {
    const donorsCollection = primary.getCollection(BasicServerlessTest.kConfigSplitDonorsNS);
    return donorsCollection.deleteOne({"_id": migrationId}, {w: "majority"});
}

function assertMigrationState(primary, migrationId, state) {
    const migrationDoc = findMigration(primary, migrationId);
    assert(migrationDoc);

    if (migrationDoc.state === 'aborted') {
        print(tojson(migrationDoc));
    }

    assert.eq(migrationDoc.state, state);
}
