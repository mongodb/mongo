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
    getTenantMigrationAccessBlocker({donorNode, tenantId}) {
        const res = donorNode.adminCommand({serverStatus: 1});
        assert.commandWorked(res);

        const tenantMigrationAccessBlocker = res.tenantMigrationAccessBlocker;

        if (!tenantMigrationAccessBlocker) {
            return undefined;
        }

        tenantMigrationAccessBlocker.donor =
            tenantMigrationAccessBlocker[tenantId] && tenantMigrationAccessBlocker[tenantId].donor;

        return tenantMigrationAccessBlocker;
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
            const allAccessBlockersRemoved = tenantIds.every(
                id => this.getTenantMigrationAccessBlocker({donorNode: node, id}) == null);

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
                        id => this.getTenantMigrationAccessBlocker({donorNode: node, id}) != null);
                    status.push(`access blockers to be removed (${tenantsWithBlockers})`);
                }
            }
            return donorDocumentDeleted && allAccessBlockersRemoved;
        }));
    }

    forgetShardSplit(migrationId) {
        jsTestLog("Running forgetShardSplit command");
        const cmdObj = {forgetShardSplit: 1, migrationId: migrationId};
        const primary = this.donor.getPrimary();
        return assert.commandWorked(primary.getDB('admin').runCommand(cmdObj));
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
                this.getTenantMigrationAccessBlocker({donorNode: donorPrimary, tenantId}).donor;
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

function findMigration(primary, uuid) {
    const donorsCollection = primary.getCollection(BasicServerlessTest.kConfigSplitDonorsNS);
    return donorsCollection.findOne({"_id": uuid});
}

function cleanupMigrationDocument(primary, uuid) {
    const donorsCollection = primary.getCollection(BasicServerlessTest.kConfigSplitDonorsNS);
    return donorsCollection.deleteOne({"_id": uuid}, {w: "majority"});
}

function assertMigrationState(primary, uuid, state) {
    const migrationDoc = findMigration(primary, uuid);
    assert(migrationDoc);

    if (migrationDoc.state === 'aborted') {
        print(tojson(migrationDoc));
    }

    assert.eq(migrationDoc.state, state);
}
