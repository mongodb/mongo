load("jstests/replsets/rslib.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");

const runForgetShardSplitAsync = function(primaryHost, migrationIdString) {
    const primary = new Mongo(primaryHost);
    return primary.adminCommand({forgetShardSplit: 1, migrationId: UUID(migrationIdString)});
};

const runAbortShardSplitAsync = function(primaryHost, migrationIdString) {
    const primary = new Mongo(primaryHost);
    return primary.adminCommand({abortShardSplit: 1, migrationId: UUID(migrationIdString)});
};

/**
 * Convert arguments passed through the Thread interface and calls runShardSplitCommand.
 */
const runCommitSplitThreadWrapper = function(rstArgs,
                                             migrationIdString,
                                             tenantIds,
                                             recipientTagName,
                                             recipientSetName,
                                             retryOnRetryableErrors,
                                             enableDonorStartMigrationFsync) {
    load("jstests/replsets/rslib.js");
    load("jstests/serverless/libs/basic_serverless_test.js");

    const donorRst = createRst(rstArgs, true);

    const commitShardSplitCmdObj = {
        commitShardSplit: 1,
        migrationId: UUID(migrationIdString),
        tenantIds: tenantIds,
        recipientTagName: recipientTagName,
        recipientSetName: recipientSetName
    };

    jsTestLog(`Running async split command ${tojson(commitShardSplitCmdObj)}`);

    return runShardSplitCommand(
        donorRst, commitShardSplitCmdObj, retryOnRetryableErrors, enableDonorStartMigrationFsync);
};

const runShardSplitCommand = function(
    replicaSet, cmdObj, retryOnRetryableErrors, enableDonorStartMigrationFsync) {
    let res;
    if (enableDonorStartMigrationFsync) {
        replicaSet.awaitLastOpCommitted();
        assert.commandWorked(replicaSet.getPrimary().adminCommand({fsync: 1}));
    }

    assert.soon(() => {
        try {
            const primary = replicaSet.getPrimary();
            // Note: assert.commandWorked() considers command responses with embedded
            // writeErrors and WriteConcernErrors as a failure even if the command returned
            // "ok: 1". And, admin commands(like, donorStartMigration)
            // doesn't generate writeConcernErros or WriteErrors. So, it's safe to wrap up
            // run() with assert.commandWorked() here. However, in few scenarios, like
            // Mongo.prototype.recordRerouteDueToTenantMigration(), it's not safe to wrap up
            // run() with commandWorked() as retrying on retryable writeConcernErrors can
            // cause the retry attempt to fail with writeErrors.
            res = undefined;
            res = primary.adminCommand(cmdObj);
            assert.commandWorked(res);
            return true;
        } catch (e) {
            if (retryOnRetryableErrors && isRetryableError(e)) {
                jsTestLog(`runShardSplitCommand retryable error. Command: ${
                    tojson(cmdObj)}, reply: ${tojson(res)}`);

                return false;
            }

            // If res is defined, return true to exit assert.soon and return res to the caller.
            // Otherwise rethrow e to propagate it to the caller.
            if (res)
                return true;

            throw e;
        }
    }, "failed to retry commitShardSplit", 10 * 1000, 1 * 1000);
    return res;
};

/**
 * Utility class to run shard split operations.
 */
class ShardSplitOperation {
    constructor(basicServerlessTest, recipientSetName, recipientTagName, tenantIds, migrationId) {
        this.basicServerlessTest = basicServerlessTest;
        this.donorSet = basicServerlessTest.donor;
        this.recipientTagName = recipientTagName;
        this.recipientSetName = recipientSetName;
        this.tenantIds = tenantIds;
        this.migrationId = migrationId;
    }

    /**
     * Starts a shard split synchronously.
     */

    commit({retryOnRetryableErrors} = {retryOnRetryableErrors: false},
           {enableDonorStartMigrationFsync} = {enableDonorStartMigrationFsync: false}) {
        jsTestLog("Running commit command");
        const localCmdObj = {
            commitShardSplit: 1,
            migrationId: this.migrationId,
            tenantIds: this.tenantIds,
            recipientTagName: this.recipientTagName,
            recipientSetName: this.recipientSetName
        };

        return runShardSplitCommand(
            this.donorSet, localCmdObj, retryOnRetryableErrors, enableDonorStartMigrationFsync);
    }

    /**
     * Starts a shard split asynchronously and returns the Thread that runs it.
     * @returns the Thread running the commitShardSplit command.
     */
    commitAsync({retryOnRetryableErrors, enableDonorStartMigrationFsync} = {
        retryOnRetryableErrors: false,
        enableDonorStartMigrationFsync: false
    }) {
        const donorRst = createRstArgs(this.donorSet);
        const migrationIdString = extractUUIDFromObject(this.migrationId);

        const thread = new Thread(runCommitSplitThreadWrapper,
                                  donorRst,
                                  migrationIdString,
                                  this.tenantIds,
                                  this.recipientTagName,
                                  this.recipientSetName,
                                  retryOnRetryableErrors,
                                  enableDonorStartMigrationFsync);
        thread.start();

        return thread;
    }

    /**
     * Forgets a shard split synchronously.
     */
    forget() {
        jsTestLog("Running forgetShardSplit command");

        this.basicServerlessTest.removeRecipientNodesFromDonor();
        const donorRstArgs = createRstArgs(this.donorSet);
        this.basicServerlessTest.removeRecipientsFromRstArgs(donorRstArgs);
        const donorSet = createRst(donorRstArgs, true);

        const cmdObj = {forgetShardSplit: 1, migrationId: this.migrationId};
        assert.commandWorked(runShardSplitCommand(donorSet,
                                                  cmdObj,
                                                  true /* retryableOnErrors */,
                                                  false /*enableDonorStartMigrationFsync*/));
    }

    forgetAsync() {
        jsTestLog("Running forgetShardSplit command asynchronously");

        const primary = this.basicServerlessTest.getDonorPrimary();
        const migrationIdString = extractUUIDFromObject(this.migrationId);

        const forgetMigrationThread =
            new Thread(runForgetShardSplitAsync, primary.host, migrationIdString);

        forgetMigrationThread.start();

        return forgetMigrationThread;
    }

    /**
     * Send an abortShardSplit command asynchronously and returns the Thread that runs it.
     * @returns the Thread running the abortShardSplit command.
     */
    abortAsync() {
        jsTestLog("Running abortShardSplit command asynchronously");
        const primary = this.basicServerlessTest.getDonorPrimary();
        const migrationIdString = extractUUIDFromObject(this.migrationId);

        const abortShardSplitThread =
            new Thread(runAbortShardSplitAsync, primary.host, migrationIdString);

        abortShardSplitThread.start();

        return abortShardSplitThread;
    }

    /**
     * Aborts a shard split synchronously.
     */
    abort() {
        jsTestLog("Running abort command");
        const primary = this.basicServerlessTest.getDonorPrimary();
        const admin = primary.getDB("admin");

        return admin.runCommand({abortShardSplit: 1, migrationId: this.migrationId});
    }
}

/**
 * Utility class to create a ReplicaSetTest that provides functionnality to run a shard split
 * operation.
 */
class BasicServerlessTest {
    constructor({
        recipientTagName,
        recipientSetName,
        quickGarbageCollection = false,
        nodeOptions,
        allowStaleReadsOnDonor = false,
        initiateWithShortElectionTimeout = false
    }) {
        nodeOptions = nodeOptions || {};
        if (quickGarbageCollection) {
            nodeOptions["setParameter"] = nodeOptions["setParameter"] || {};
            Object.assign(nodeOptions["setParameter"],
                          {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1});
        }
        if (allowStaleReadsOnDonor) {
            nodeOptions["setParameter"]["failpoint.tenantMigrationDonorAllowsNonTimestampedReads"] =
                tojson({mode: 'alwaysOn'});
        }
        this.donor = new ReplSetTest({name: "donor", nodes: 3, serverless: true, nodeOptions});
        this.donor.startSet();
        if (initiateWithShortElectionTimeout) {
            this.initiateWithShortElectionTimeout();
        } else {
            this.donor.initiate();
        }

        this.recipientTagName = recipientTagName;
        this.recipientSetName = recipientSetName;
        this.recipientNodes = [];
    }

    initiateWithShortElectionTimeout() {
        let config = this.donor.getReplSetConfig();
        config.settings = config.settings || {};
        config.settings["electionTimeoutMillis"] = 500;
        this.donor.initiate(config);
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
     * @returns the created shard split operation object.
     */
    createSplitOperation(tenantIds) {
        const migrationId = UUID();
        jsTestLog("Asserting no state document exist before command");
        assert.isnull(findSplitOperation(this.donor.getPrimary(), migrationId));

        return new ShardSplitOperation(
            this, this.recipientSetName, this.recipientTagName, tenantIds, migrationId);
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
                hidden: true,
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
                MongoRunner.stopMongod(node, undefined, {skipValidation: true});
            }
        });
    }

    /**
     * Crafts a tenant database name.
     * @param {tenantId} tenant ID to be used for the tenant database name
     * @param {dbName} name of the database to be used for the tenant database name
     * @returns crafted databased name using a tenantId and a database name.
     */
    tenantDB(tenantId, dbName) {
        return `${tenantId}_${dbName}`;
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
     * from the 'shardSplitDonors' namespace, and all access blockers have been removed.
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
            const allAccessBlockersRemoved = tenantIds.every(
                id => BasicServerlessTest.getTenantMigrationAccessBlocker({node, id}) == null);

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
                    const tenantsWithBlockers =
                        tenantIds.filter(id => BasicServerlessTest.getTenantMigrationAccessBlocker(
                                                   {node, id}) != null);
                    status.push(`access blockers to be removed (${tenantsWithBlockers})`);
                }
            }
            return donorDocumentDeleted && allAccessBlockersRemoved;
        }),
                    "tenant access blockers weren't removed",
                    60 * 1000,
                    1 * 1000);
    }

    /**
     * Remove recipient nodes from the donor.nodes of the BasicServerlessTest.
     */
    removeRecipientNodesFromDonor() {
        jsTestLog("Removing recipient nodes from the donor.");
        this.donor.nodes = this.donor.nodes.filter(node => !this.recipientNodes.includes(node));
        this.donor.ports =
            this.donor.ports.filter(port => !this.recipientNodes.some(node => node.port === port));
    }

    /**
     * Remove the recipient nodes from the donor's config memberset and calls replSetReconfig on the
     * updated local config. It does not need to be called after a successfull split as the service
     * reconfig itself in that case.
     */
    reconfigDonorSetAfterSplit() {
        const primary = this.donor.getPrimary();
        const config = this.donor.getReplSetConfigFromNode();
        config.version++;

        let donorNodeHosts = [];
        this.donor.nodes.forEach(node => {
            donorNodeHosts.push("" + node.host);
        });

        // remove recipient nodes and config.
        config.members =
            config.members.filter(member => donorNodeHosts.some(node => node === member.host));
        delete config.recipientConfig;

        assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
    }

    /*
     * Look up tenant access blockers for the given tenant ids and will check, based upon the
     * expected state the access blockers are expected to be, that the different fields are
     * properly set such as `blockOpTime`, `abortOpTime` or `commitOpTime`.
     * @param {migrationId} the current shard split id.
     * @param {tenantIds} tenant ids of the shard split.
     * @param {expectedState} expected state the tenant access blocker to be in.
     */
    validateTenantAccessBlockers(migrationId, tenantIds, expectedState) {
        let donorPrimary = this.donor.getPrimary();
        const stateDoc = findSplitOperation(donorPrimary, migrationId);
        assert.soon(() => tenantIds.every(tenantId => {
            const donorMtab =
                BasicServerlessTest.getTenantMigrationAccessBlocker({node: donorPrimary, tenantId})
                    .donor;
            const tenantAccessBlockersBlockRW = donorMtab.state == expectedState;
            const tenantAccessBlockersBlockTimestamp =
                bsonWoCompare(donorMtab.blockTimestamp, stateDoc.blockOpTime.ts) == 0;

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
        }),
                    "failed to validate tenant access blockers",
                    10 * 1000,
                    1 * 100);
    }

    /**
     * After calling the forgetShardSplit command, wait for the tenant access blockers to be removed
     * then remove and stop the recipient nodes from the donor set.
     * @param {migrationId} migration id of the committed shard split operation.
     * @param {tenantIds}  tenant IDs that were used for the split operation.
     */
    cleanupSuccesfulCommitted(migrationId, tenantIds) {
        this.waitForGarbageCollection(migrationId, tenantIds);
        this.removeAndStopRecipientNodes();
    }

    /**
     * After calling the forgetShardSplit command, wait for the tenant access blockers to be removed
     * then remove and stop the recipient nodes from the donor set and test and finally apply the
     * new config once the split has been cleaned up.
     * @param {migrationId} migration id of the committed shard split operation.
     * @param {tenantIds}  tenant IDs that were used for the split operation.
     */
    cleanupSuccesfulAborted(migrationId, tenantIds) {
        this.waitForGarbageCollection(migrationId, tenantIds);
        this.removeAndStopRecipientNodes();
        this.reconfigDonorSetAfterSplit();
    }

    /*
     * Lookup and return the tenant migration access blocker on a node for the given tenant.
     * @param {donorNode} donor node on which the request will be sent.
     * @param {tenantId} tenant id to lookup for tenant access blockers.
     */
    static getTenantMigrationAccessBlocker({node, tenantId}) {
        const res = node.adminCommand({serverStatus: 1});
        assert.commandWorked(res);

        const tenantMigrationAccessBlocker = res.tenantMigrationAccessBlocker;

        if (!tenantMigrationAccessBlocker) {
            return undefined;
        }

        tenantMigrationAccessBlocker.donor =
            tenantMigrationAccessBlocker[tenantId] && tenantMigrationAccessBlocker[tenantId].donor;

        return tenantMigrationAccessBlocker;
    }

    /**
     * Returns the number of reads on the given node that were blocked due to shard split
     * for the given tenant.
     */
    static getNumBlockedReads(donorNode, tenantId) {
        const mtab =
            BasicServerlessTest.getTenantMigrationAccessBlocker({node: donorNode, tenantId});
        if (!mtab) {
            return 0;
        }
        return mtab.donor.numBlockedReads;
    }

    /**
     * Returns the number of writes on the given donor node that were blocked due to shard split
     * for the given tenant.
     */
    static getNumBlockedWrites(donorNode, tenantId) {
        const mtab =
            BasicServerlessTest.getTenantMigrationAccessBlocker({node: donorNode, tenantId});
        if (!mtab) {
            return 0;
        }
        return mtab.donor.numBlockedWrites;
    }

    /**
     * Asserts that the TenantMigrationAccessBlocker for the given tenant on the given node has the
     * expected statistics.
     */
    static checkShardSplitAccessBlocker(node, tenantId, {
        numBlockedWrites = 0,
        numBlockedReads = 0,
        numTenantMigrationCommittedErrors = 0,
        numTenantMigrationAbortedErrors = 0
    }) {
        const mtab = BasicServerlessTest.getTenantMigrationAccessBlocker({node, tenantId}).donor;
        if (!mtab) {
            assert.eq(0, numBlockedWrites);
            assert.eq(0, numTenantMigrationCommittedErrors);
            assert.eq(0, numTenantMigrationAbortedErrors);
            return;
        }

        assert.eq(mtab.numBlockedReads, numBlockedReads, tojson(mtab));
        assert.eq(mtab.numBlockedWrites, numBlockedWrites, tojson(mtab));
        assert.eq(mtab.numTenantMigrationCommittedErrors,
                  numTenantMigrationCommittedErrors,
                  tojson(mtab));
        assert.eq(
            mtab.numTenantMigrationAbortedErrors, numTenantMigrationAbortedErrors, tojson(mtab));
    }

    /**
     * Get the current donor primary by ignoring all the recipient nodes from the current donor set.
     */
    getDonorPrimary() {
        const donorRstArgs = createRstArgs(this.donor);
        this.removeRecipientsFromRstArgs(donorRstArgs);
        const donorRst = createRst(donorRstArgs, true);
        return donorRst.getPrimary();
    }

    /**
     * @returns A new ReplSetTest fixture representing the recipient set.
     */
    getRecipient() {
        const recipientRstArgs = createRstArgs(this.donor);
        recipientRstArgs.nodeHosts = this.recipientNodes.map(node => node.host);
        assert(recipientRstArgs.nodeHosts.length >= 3);
        return createRst(recipientRstArgs, true);
    }
}

BasicServerlessTest.kConfigSplitDonorsNS = "config.shardSplitDonors";
BasicServerlessTest.DonorState = {
    kUninitialized: "uninitialized",
    kBlocking: "blocking",
    kCommitted: "committed",
    kAborted: "aborted"
};

function findSplitOperation(primary, migrationId) {
    const donorsCollection = primary.getCollection(BasicServerlessTest.kConfigSplitDonorsNS);
    return donorsCollection.findOne({"_id": migrationId});
}

function cleanupMigrationDocument(primary, migrationId) {
    const donorsCollection = primary.getCollection(BasicServerlessTest.kConfigSplitDonorsNS);
    return donorsCollection.deleteOne({"_id": migrationId}, {w: "majority"});
}

function assertMigrationState(primary, migrationId, state) {
    const migrationDoc = findSplitOperation(primary, migrationId);
    assert(migrationDoc);

    if (migrationDoc.state === 'aborted') {
        print(tojson(migrationDoc));
    }

    // If transitioning to "blocking", prove that we wrote that fact at the blockOpTime.
    if (state === "blocking") {
        const oplogEntry =
            primary.getDB("local").oplog.rs.find({ts: migrationDoc.blockOpTime.ts}).next();
        assert.neq(null, oplogEntry.o, oplogEntry);
        assert.neq(null, oplogEntry.o.state, oplogEntry);
        assert.eq(oplogEntry.o.state, state, oplogEntry);
    }

    assert.eq(migrationDoc.state, state);
}
