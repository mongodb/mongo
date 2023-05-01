import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeX509OptionsForTest} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/replsets/rslib.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");

function runForgetShardSplitAsync(primaryHost, migrationIdString) {
    const primary = new Mongo(primaryHost);
    return primary.adminCommand({forgetShardSplit: 1, migrationId: UUID(migrationIdString)});
}

function runAbortShardSplitAsync(primaryHost, migrationIdString) {
    const primary = new Mongo(primaryHost);
    return primary.adminCommand({abortShardSplit: 1, migrationId: UUID(migrationIdString)});
}

/*
 * Connects to a replica set and runs write operation, returning the results.
 * @param {rstArgs} replicaSetArgs for the replica set to connect to.
 * @param {tenantIds} perform a write operation for each tenantId.
 */
export function doWriteOperations(rstArgs, tenantIds) {
    load("jstests/replsets/rslib.js");

    const donorRst = createRst(rstArgs, true);
    const donorPrimary = donorRst.getPrimary();

    const writeResults = [];
    // TenantIds is an array of ObjectId which does not serialize correctly when passed through the
    // Thread constructor. We pass tenantIds converted into a json form (stringfield) to this
    // function and then use `eval()` to rebuild into an array of ObjectId.
    const tenantIdsObjs = eval(tenantIds);

    tenantIdsObjs.forEach(id => {
        const kDbName = `${id.str}_testDb`;
        const kCollName = "testColl";
        const kNs = `${kDbName}.${kCollName}`;

        const res = donorPrimary.getDB(kDbName)
                        .getCollection(kNs)
                        .insert([{_id: 0, x: 0}, {_id: 1, x: 1}, {_id: 2, x: 2}],
                                {writeConcern: {w: "majority"}})
                        .getRawResponse();
        if (res.writeErrors.length > 0) {
            writeResults.push(res.writeErrors[0].code);
        } else {
            // Push OK
            writeResults.push(0);
        }
    });

    return writeResults;
}

export function addRecipientNodes({rst, numNodes, recipientTagName, nodeOptions}) {
    numNodes = numNodes || 3;  // default to three nodes
    const recipientNodes = [];
    const options = Object.assign({}, makeX509OptionsForTest().donor, nodeOptions);
    jsTestLog(`Adding ${numNodes} non-voting recipient nodes to donor`);
    for (let i = 0; i < numNodes; ++i) {
        recipientNodes.push(rst.add(options));
    }

    const primary = rst.getPrimary();
    const admin = primary.getDB('admin');
    const config = rst.getReplSetConfigFromNode();
    config.version++;

    // ensure recipient nodes are added as non-voting members
    recipientNodes.forEach(node => {
        config.members.push({
            host: node.host,
            votes: 0,
            priority: 0,
            hidden: true,
            tags: {[recipientTagName]: ObjectId().valueOf()}
        });
    });

    // reindex all members from 0
    config.members = config.members.map((member, idx) => {
        member._id = idx;
        return member;
    });

    assert.commandWorked(admin.runCommand({replSetReconfig: config}));
    recipientNodes.forEach(node => rst.waitForState(node, ReplSetTest.State.SECONDARY));

    return recipientNodes;
}

/**
 * Convert arguments passed through the Thread interface and calls runShardSplitCommand.
 */
async function runCommitSplitThreadWrapper(rstArgs,
                                           migrationIdString,
                                           tenantIds,
                                           recipientTagName,
                                           recipientSetName,
                                           retryOnRetryableErrors,
                                           enableDonorStartMigrationFsync) {
    load("jstests/replsets/rslib.js");
    const {runShardSplitCommand} = await import("jstests/serverless/libs/shard_split_test.js");

    const donorRst = createRst(rstArgs, true);
    const commitShardSplitCmdObj = {
        commitShardSplit: 1,
        migrationId: UUID(migrationIdString),
        tenantIds: eval(tenantIds),  // tenantIds were passed as a json instead of array<objectId>
        recipientTagName: recipientTagName,
        recipientSetName: recipientSetName
    };

    jsTestLog(`Running async split command ${tojson(commitShardSplitCmdObj)}`);

    return runShardSplitCommand(
        donorRst, commitShardSplitCmdObj, retryOnRetryableErrors, enableDonorStartMigrationFsync);
}

/*
 *  Wait for state document garbage collection by polling for when the document has been removed
 * from the 'shardSplitDonors' namespace, and all access blockers have been removed.
 * @param {migrationId} id that was used for the commitShardSplit command.
 * @param {tenantIds} tenant ids of the shard split.
 */
export function waitForGarbageCollectionForSplit(donorNodes, migrationId, tenantIds) {
    jsTestLog("Wait for garbage collection");
    assert.soon(() => donorNodes.every(node => {
        const donorDocumentDeleted =
            node.getCollection(ShardSplitTest.kConfigSplitDonorsNS).count({_id: migrationId}) === 0;
        const allAccessBlockersRemoved = tenantIds.every(
            id => ShardSplitTest.getTenantMigrationAccessBlocker({node, tenantId: id}) == null);

        const result = donorDocumentDeleted && allAccessBlockersRemoved;
        if (!result) {
            const status = [];
            if (!donorDocumentDeleted) {
                status.push(`donor document to be deleted (docCount=${
                    node.getCollection(ShardSplitTest.kConfigSplitDonorsNS).count({
                        _id: migrationId
                    })})`);
            }

            if (!allAccessBlockersRemoved) {
                const tenantsWithBlockers =
                    tenantIds.filter(id => ShardSplitTest.getTenantMigrationAccessBlocker(
                                               {node, tenantId: id}) != null);
                status.push(`access blockers to be removed (${tenantsWithBlockers})`);
            }
        }
        return donorDocumentDeleted && allAccessBlockersRemoved;
    }),
                "tenant access blockers weren't removed",
                60 * 1000,
                1 * 1000);
}

export function commitSplitAsync({
    rst,
    tenantIds,
    recipientTagName,
    recipientSetName,
    migrationId,
    retryOnRetryableErrors,
    enableDonorStartMigrationFsync
} = {
    retryOnRetryableErrors: false,
    enableDonorStartMigrationFsync: false
}) {
    jsTestLog("Running commitAsync command");

    const rstArgs = createRstArgs(rst);
    const migrationIdString = extractUUIDFromObject(migrationId);

    const thread = new Thread(runCommitSplitThreadWrapper,
                              rstArgs,
                              migrationIdString,
                              tojson(tenantIds),
                              recipientTagName,
                              recipientSetName,
                              retryOnRetryableErrors,
                              enableDonorStartMigrationFsync);
    thread.start();

    return thread;
}

export function runShardSplitCommand(
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
            // In some tests we expects the command to fail due to a network error. We want to
            // catch the error OR the unhandled exception here and return the error to the
            // caller to assert on the result. Otherwise if this is not a network exception
            // it will be caught in the outter catch and either be retried or thrown.
            res = executeNoThrowNetworkError(() => primary.adminCommand(cmdObj));
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
}

/**
 * Utility class to run shard split operations.
 */
class ShardSplitOperation {
    constructor(shardSplitTest, recipientSetName, recipientTagName, tenantIds, migrationId) {
        this.shardSplitTest = shardSplitTest;
        this.donorSet = shardSplitTest.donor;
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
        return commitSplitAsync({
            rst: this.donorSet,
            tenantIds: this.tenantIds,
            recipientTagName: this.recipientTagName,
            recipientSetName: this.recipientSetName,
            migrationId: this.migrationId,
            retryOnRetryableErrors,
            enableDonorStartMigrationFsync
        });
    }

    /**
     * Forgets a shard split synchronously.
     */
    forget() {
        jsTestLog("Running forgetShardSplit command");

        this.shardSplitTest.removeRecipientNodesFromDonor();
        const donorRstArgs = createRstArgs(this.donorSet);
        this.shardSplitTest.removeRecipientsFromRstArgs(donorRstArgs);
        const donorSet = createRst(donorRstArgs, true);

        const cmdObj = {forgetShardSplit: 1, migrationId: this.migrationId};
        assert.commandWorked(runShardSplitCommand(donorSet,
                                                  cmdObj,
                                                  true /* retryableOnErrors */,
                                                  false /*enableDonorStartMigrationFsync*/));
    }

    forgetAsync() {
        jsTestLog("Running forgetShardSplit command asynchronously");

        const primary = this.shardSplitTest.getDonorPrimary();
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
        const primary = this.shardSplitTest.getDonorPrimary();
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
        const primary = this.shardSplitTest.getDonorPrimary();
        const admin = primary.getDB("admin");

        return admin.runCommand({abortShardSplit: 1, migrationId: this.migrationId});
    }
}

/**
 * Utility class to create a ReplicaSetTest that provides functionnality to run a shard split
 * operation.
 */
export class ShardSplitTest {
    constructor({
        recipientTagName = "recipientNode",
        recipientSetName = "recipientSetName",
        quickGarbageCollection = false,
        donorRst,
        nodeOptions,
        allowStaleReadsOnDonor = false,
    } = {}) {
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
        if (donorRst) {
            this.donor = donorRst;
        } else {
            this.donor = new ReplSetTest({name: "donor", nodes: 3, serverless: true, nodeOptions});
            this.donor.startSet();
            this.donor.initiate();
        }

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
    addRecipientNodes({numNodes, nodeOptions} = {}) {
        if (this.recipientNodes.length > 0) {
            throw new Error("Recipient nodes may only be added once");
        }

        this.recipientNodes = addRecipientNodes(
            {rst: this.donor, numNodes, nodeOptions, recipientTagName: this.recipientTagName});
    }

    /*
     * Add recipient nodes to the current donor set, and wait for them to become ready.
     * @param {numNodes} indicates the number of recipient nodes to be added.
     */
    addAndAwaitRecipientNodes(numNodes) {
        this.addRecipientNodes({numNodes});
        this.donor.awaitSecondaryNodes();
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
     *  Wait for state document garbage collection by polling for when the document has been
     * removed from the 'shardSplitDonors' namespace, and all access blockers have been removed.
     * @param {migrationId} id that was used for the commitShardSplit command.
     * @param {tenantIds} tenant ids of the shard split.
     */
    waitForGarbageCollection(migrationId, tenantIds) {
        return waitForGarbageCollectionForSplit(this.donor.nodes, migrationId, tenantIds);
    }

    /**
     * Remove recipient nodes from the donor.nodes of ShardSplitTest.
     */
    removeRecipientNodesFromDonor() {
        jsTestLog("Removing recipient nodes from the donor.");
        this.donor.nodes = this.donor.nodes.filter(node => !this.recipientNodes.includes(node));
        this.donor.ports =
            this.donor.ports.filter(port => !this.recipientNodes.some(node => node.port === port));
    }

    /**
     * Remove the recipient nodes from the donor's config memberset and calls replSetReconfig on
     * the updated local config. It does not need to be called after a successfull split as the
     * service reconfig itself in that case.
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
                ShardSplitTest
                    .getTenantMigrationAccessBlocker({node: donorPrimary, tenantId: tenantId})
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
     * After calling the forgetShardSplit command, wait for the tenant access blockers to be
     * removed then remove and stop the recipient nodes from the donor set.
     * @param {migrationId} migration id of the committed shard split operation.
     * @param {tenantIds}  tenant IDs that were used for the split operation.
     */
    cleanupSuccesfulCommitted(migrationId, tenantIds) {
        this.waitForGarbageCollection(migrationId, tenantIds);
        this.removeAndStopRecipientNodes();
    }

    /**
     * After calling the forgetShardSplit command, wait for the tenant access blockers to be
     * removed then remove and stop the recipient nodes from the donor set and test and finally
     * apply the new config once the split has been cleaned up.
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
     * @param {tenantId} tenant id (ObjectId) to lookup for tenant access blockers.
     */
    static getTenantMigrationAccessBlocker({node, tenantId}) {
        const res = node.adminCommand({serverStatus: 1});
        assert.commandWorked(res);

        const tenantMigrationAccessBlocker = res.tenantMigrationAccessBlocker;

        if (!tenantMigrationAccessBlocker) {
            return undefined;
        }

        tenantMigrationAccessBlocker.donor = tenantMigrationAccessBlocker[tenantId.str] &&
            tenantMigrationAccessBlocker[tenantId.str].donor;

        return tenantMigrationAccessBlocker;
    }

    /**
     * Returns the number of reads on the given node that were blocked due to shard split
     * for the given tenant.
     */
    static getNumBlockedReads(donorNode, tenantId) {
        const mtab = ShardSplitTest.getTenantMigrationAccessBlocker({node: donorNode, tenantId});
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
        const mtab = ShardSplitTest.getTenantMigrationAccessBlocker({node: donorNode, tenantId});
        if (!mtab) {
            return 0;
        }
        return mtab.donor.numBlockedWrites;
    }

    /**
     * Asserts that the TenantMigrationAccessBlocker for the given tenant on the given node has
     * the expected statistics.
     */
    static checkShardSplitAccessBlocker(node, tenantId, {
        numBlockedWrites = 0,
        numBlockedReads = 0,
        numTenantMigrationCommittedErrors = 0,
        numTenantMigrationAbortedErrors = 0
    }) {
        const mtab = ShardSplitTest.getTenantMigrationAccessBlocker({node, tenantId}).donor;
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
     * Get the current donor primary by ignoring all the recipient nodes from the current donor
     * set.
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

    /**
     * @returns An array of recipient nodes.
     */
    getRecipientNodes() {
        return this.recipientNodes;
    }

    /**
     * @returns An array of donor nodes.
     */
    getDonorNodes() {
        return this.donor.nodes.filter(node => !this.recipientNodes.includes(node));
    }
}

ShardSplitTest.kConfigSplitDonorsNS = "config.shardSplitDonors";
ShardSplitTest.DonorState = {
    kUninitialized: "uninitialized",
    kBlocking: "blocking",
    kCommitted: "committed",
    kAborted: "aborted"
};

export function findSplitOperation(primary, migrationId) {
    const donorsCollection = primary.getCollection(ShardSplitTest.kConfigSplitDonorsNS);
    return donorsCollection.findOne({"_id": migrationId});
}

export function assertMigrationState(primary, migrationId, state) {
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
