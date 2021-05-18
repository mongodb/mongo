/**
 * Wrapper around ReplSetTest for testing tenant migration behavior.
 */

"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/replsets/libs/tenant_migration_util.js");

/**
 * This fixture allows the user to optionally pass in a custom ReplSetTest for the donor and
 * recipient replica sets, to be used for the test.
 *
 * If the caller does not provide their own replica set, a two node replset will be initialized
 * instead, with all nodes running the latest version.
 *
 * @param {string} [name] the name of the replica sets
 * @param {boolean} [enableRecipientTesting] whether recipient would actually migrate tenant data
 * @param {Object} [donorRst] the ReplSetTest instance to adopt for the donor
 * @param {Object} [recipientRst] the ReplSetTest instance to adopt for the recipient
 * @param {Object} [sharedOptions] an object that can contain 'nodes' <number>, the number of nodes
 *     each RST will contain, and 'setParameter' <object>, an object with various server parameters.
 * @param {boolean} [allowDonorReadAfterMigration] whether donor would allow reads after a committed
 *     migration.
 * @param {boolean} [initiateRstWithHighElectionTimeout] whether donor and recipient replica sets
 *     should be initiated with high election timeout.
 */
function TenantMigrationTest({
    name = "TenantMigrationTest",
    enableRecipientTesting = true,
    donorRst,
    recipientRst,
    sharedOptions = {},
    // Default this to true so it is easier for data consistency checks.
    allowStaleReadsOnDonor = true,
    initiateRstWithHighElectionTimeout = true
}) {
    const donorPassedIn = (donorRst !== undefined);
    const recipientPassedIn = (recipientRst !== undefined);

    const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();
    const migrationCertificates = TenantMigrationUtil.makeMigrationCertificatesForTest();

    const nodes = sharedOptions.nodes || 2;
    let setParameterOpts = sharedOptions.setParameter || {};

    donorRst = donorPassedIn ? donorRst : performSetUp(true /* isDonor */);
    recipientRst = recipientPassedIn ? recipientRst : performSetUp(false /* isDonor */);

    donorRst.asCluster(donorRst.nodes, () => {
        donorRst.getPrimary();
        donorRst.awaitReplication();
        TenantMigrationUtil.createTenantMigrationRecipientRoleIfNotExist(donorRst);
    });

    recipientRst.asCluster(recipientRst.nodes, () => {
        recipientRst.getPrimary();
        recipientRst.awaitReplication();
        TenantMigrationUtil.createTenantMigrationDonorRoleIfNotExist(recipientRst);
    });

    /**
     * Creates a ReplSetTest instance. The repl set will have 2 nodes if not otherwise specified.
     */
    function performSetUp(isDonor) {
        if (TestData.logComponentVerbosity) {
            setParameterOpts["logComponentVerbosity"] =
                tojsononeline(TestData.logComponentVerbosity);
        }

        if (!(isDonor || enableRecipientTesting)) {
            setParameterOpts["failpoint.returnResponseOkForRecipientSyncDataCmd"] =
                tojson({mode: 'alwaysOn'});
        }

        if (allowStaleReadsOnDonor) {
            setParameterOpts["failpoint.tenantMigrationDonorAllowsNonTimestampedReads"] =
                tojson({mode: 'alwaysOn'});
        }

        let nodeOptions = isDonor ? migrationX509Options.donor : migrationX509Options.recipient;
        nodeOptions["setParameter"] = setParameterOpts;

        const rstName = `${name}_${(isDonor ? "donor" : "recipient")}`;
        const rst = new ReplSetTest({name: rstName, nodes, nodeOptions});
        rst.startSet();
        if (initiateRstWithHighElectionTimeout) {
            rst.initiateWithHighElectionTimeout();
        } else {
            rst.initiate();
        }

        return rst;
    }

    /**
     * Returns whether tenant migration commands are supported.
     */
    this.isFeatureFlagEnabled = function() {
        function supportsTenantMigrations(rst) {
            const conn = rst.getPrimary();
            return rst.asCluster(conn, () => {
                return assert
                    .commandWorked(
                        conn.adminCommand({getParameter: 1, featureFlagTenantMigrations: 1}))
                    .featureFlagTenantMigrations.value;
            });
        }
        const retVal = (supportsTenantMigrations(this.getDonorRst()) &&
                        supportsTenantMigrations(this.getRecipientRst()));
        if (!retVal) {
            jsTestLog("At least one of the donor or recipient replica sets do not support tenant " +
                      "migration commands. Terminating any replica sets started by the " +
                      "TenantMigrationTest fixture.");
            // Stop any replica sets started by the TenantMigrationTest fixture.
            this.stop();
        }
        return retVal;
    };

    /**
     * Runs a tenant migration with the given migration options and waits for the migration to be
     * committed or aborted.
     *
     * Returns the result of the initial donorStartMigration if it was unsuccessful. Otherwise,
     * returns the command response containing the migration state on the donor after the migration
     * has completed.
     */
    this.runMigration = function(
        migrationOpts, retryOnRetryableErrors = false, automaticForgetMigration = true) {
        const startRes = this.startMigration(migrationOpts, retryOnRetryableErrors);
        if (!startRes.ok) {
            return startRes;
        }

        const completeRes = this.waitForMigrationToComplete(migrationOpts, retryOnRetryableErrors);

        if (automaticForgetMigration &&
            (completeRes.state === TenantMigrationTest.State.kCommitted ||
             completeRes.state === TenantMigrationTest.State.kAborted)) {
            jsTestLog(`Automatically forgetting ${completeRes.state} migration with migrationId: ${
                migrationOpts.migrationIdString}`);
            this.forgetMigration(migrationOpts.migrationIdString);
        }

        return completeRes;
    };

    /**
     * Starts a tenant migration by running the 'donorStartMigration' command once.
     *
     * Returns the result of the 'donorStartMigration' command.
     */
    this.startMigration = function(migrationOpts, retryOnRetryableErrors = false) {
        return this.runDonorStartMigration(
            migrationOpts, false /* waitForMigrationToComplete */, retryOnRetryableErrors);
    };

    /**
     * Waits for a migration to complete by continuously polling the donor primary with
     * 'donorStartMigration' until the returned state is committed or aborted. Must be used with
     * startMigration, after the migration has been started for the specified tenantId.
     *
     * Returns the result of the last 'donorStartMigration' command executed.
     */
    this.waitForMigrationToComplete = function(migrationOpts, retryOnRetryableErrors = false) {
        // Assert that the migration has already been started.
        const tenantId = migrationOpts.tenantId;
        assert(this.getDonorPrimary()
                   .getCollection(TenantMigrationTest.kConfigDonorsNS)
                   .findOne({tenantId}));
        return this.runDonorStartMigration(
            migrationOpts, true /* waitForMigrationToComplete */, retryOnRetryableErrors);
    };

    /**
     * Executes the 'donorStartMigration' command on the donor primary.
     *
     * This will return on the first successful command if 'waitForMigrationToComplete' is
     * set to false. Otherwise, it will continuously poll the donor primary until the
     * migration has been committed or aborted.
     *
     * If 'retryOnRetryableErrors' is set, this function will retry if the command fails
     * with a NotPrimary or network error.
     */
    this.runDonorStartMigration = function({
        migrationIdString,
        tenantId,
        recipientConnectionString = recipientRst.getURL(),
        readPreference = {mode: "primary"},
        donorCertificateForRecipient = migrationCertificates.donorCertificateForRecipient,
        recipientCertificateForDonor = migrationCertificates.recipientCertificateForDonor,
    },
                                           waitForMigrationToComplete,
                                           retryOnRetryableErrors) {
        const cmdObj = {
            donorStartMigration: 1,
            tenantId,
            migrationId: UUID(migrationIdString),
            recipientConnectionString,
            readPreference,
            donorCertificateForRecipient,
            recipientCertificateForDonor
        };

        const stateRes = TenantMigrationUtil.runTenantMigrationCommand(
            cmdObj,
            this.getDonorRst(),
            retryOnRetryableErrors,
            stateRes => (!waitForMigrationToComplete ||
                         TenantMigrationUtil.isMigrationCompleted(stateRes)));

        // If the migration has been successfully committed, check the db hashes for the tenantId
        // between the donor and recipient.
        if (stateRes.state === TenantMigrationTest.State.kCommitted) {
            TenantMigrationUtil.checkTenantDBHashes(
                this.getDonorRst(), this.getRecipientRst(), tenantId);
        }

        return stateRes;
    };

    /**
     * Runs the donorForgetMigration command with the given migrationId and returns the response.
     *
     * If 'retryOnRetryableErrors' is set, this function will retry if the command fails with a
     * NotPrimary or network error.
     */
    this.forgetMigration = function(migrationIdString, retryOnRetryableErrors = false) {
        const cmdObj = {donorForgetMigration: 1, migrationId: UUID(migrationIdString)};
        const res = TenantMigrationUtil.runTenantMigrationCommand(
            cmdObj, this.getDonorRst(), retryOnRetryableErrors);

        // If the command succeeded, we expect that the migration is marked garbage collectable on
        // the donor and the recipient. Check the state docs for expireAt, check that the oplog
        // buffer collection has been dropped, and external keys have ttlExpiresAt.
        if (res.ok) {
            const donorPrimary = this.getDonorPrimary();
            const recipientPrimary = this.getRecipientPrimary();

            const donorStateDoc =
                donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({
                    _id: UUID(migrationIdString)
                });
            const recipientStateDoc =
                recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS).findOne({
                    _id: UUID(migrationIdString)
                });

            if (donorStateDoc) {
                assert(donorStateDoc.expireAt);
            }
            if (recipientStateDoc) {
                assert(recipientStateDoc.expireAt);
            }

            const configDBCollections = recipientPrimary.getDB('config').getCollectionNames();
            assert(!configDBCollections.includes('repl.migration.oplog_' + migrationIdString),
                   configDBCollections);

            this.getDonorRst().asCluster(donorPrimary, () => {
                const donorKeys =
                    TenantMigrationUtil.getExternalKeys(donorPrimary, UUID(migrationIdString));
                if (donorKeys.length) {
                    donorKeys.forEach(key => {
                        assert(key.hasOwnProperty("ttlExpiresAt"), tojson(key));
                    });
                }
            });

            this.getRecipientRst().asCluster(recipientPrimary, () => {
                const recipientKeys =
                    TenantMigrationUtil.getExternalKeys(recipientPrimary, UUID(migrationIdString));
                if (recipientKeys.length) {
                    recipientKeys.forEach(key => {
                        assert(key.hasOwnProperty("ttlExpiresAt"), tojson(key));
                    });
                }
            });
        }

        return res;
    };

    /**
     * Runs the donorAbortMigration command with the given migration options and returns the
     * response.
     */
    this.tryAbortMigration = function(migrationOpts, retryOnRetryableErrors = false) {
        const cmdObj = {
            donorAbortMigration: 1,
            migrationId: UUID(migrationOpts.migrationIdString),
        };
        return TenantMigrationUtil.runTenantMigrationCommand(
            cmdObj, this.getDonorRst(), retryOnRetryableErrors);
    };

    /**
     * Asserts that durable and in-memory state for the migration 'migrationId' and 'tenantId' is
     * eventually deleted from the given nodes.
     */
    this.waitForMigrationGarbageCollection = function(
        migrationId, tenantId, donorNodes, recipientNodes) {
        donorNodes = donorNodes || donorRst.nodes;
        recipientNodes = recipientNodes || recipientRst.nodes;

        donorNodes.forEach(node => {
            const configDonorsColl = node.getCollection(TenantMigrationTest.kConfigDonorsNS);
            assert.soon(() => 0 === configDonorsColl.count({_id: migrationId}), tojson(node));

            let mtabs;
            assert.soon(() => {
                mtabs = assert.commandWorked(node.adminCommand({serverStatus: 1}))
                            .tenantMigrationAccessBlocker;
                return !mtabs || !mtabs[tenantId];
            }, tojson(mtabs));
        });

        recipientNodes.forEach(node => {
            const configRecipientsColl =
                node.getCollection(TenantMigrationTest.kConfigRecipientsNS);
            assert.soon(() => 0 === configRecipientsColl.count({_id: migrationId}));
        });
    };

    /**
     * Asserts that the migration 'migrationId' and 'tenantId' eventually goes to the
     * expected state on all the given donor nodes.
     */
    this.waitForDonorNodesToReachState = function(nodes, migrationId, tenantId, expectedState) {
        nodes.forEach(node => {
            assert.soon(
                () => this.isDonorNodeInExpectedState(node, migrationId, tenantId, expectedState));
        });
    };

    /**
     * Asserts that the migration 'migrationId' and 'tenantId' is in the expected state on all the
     * given donor nodes.
     */
    this.assertDonorNodesInExpectedState = function(nodes, migrationId, tenantId, expectedState) {
        nodes.forEach(node => {
            assert(this.isDonorNodeInExpectedState(node, migrationId, tenantId, expectedState));
        });
    };

    /**
     * Returns true if the durable and in-memory state for the migration 'migrationId' and
     * 'tenantId' is in the expected state, and false otherwise.
     */
    this.isDonorNodeInExpectedState = function(node, migrationId, tenantId, expectedState) {
        const configDonorsColl =
            this.getDonorPrimary().getCollection(TenantMigrationTest.kConfigDonorsNS);
        if (configDonorsColl.findOne({_id: migrationId}).state !== expectedState) {
            return false;
        }

        const expectedAccessState = (expectedState === TenantMigrationTest.State.kCommitted)
            ? TenantMigrationTest.DonorAccessState.kReject
            : TenantMigrationTest.DonorAccessState.kAborted;
        const mtabs =
            assert.commandWorked(node.adminCommand({serverStatus: 1})).tenantMigrationAccessBlocker;
        return (mtabs[tenantId].donor.state === expectedAccessState);
    };

    /**
     * Asserts that the migration 'migrationId' and 'tenantId' eventually goes to the expected state
     * on all the given recipient nodes.
     */
    this.waitForRecipientNodesToReachState = function(
        nodes, migrationId, tenantId, expectedState, expectedAccessState) {
        nodes.forEach(node => {
            assert.soon(() => this.isRecipientNodeInExpectedState(
                            node, migrationId, tenantId, expectedState, expectedAccessState));
        });
    };

    /**
     * Asserts that the migration 'migrationId' and 'tenantId' is in the expected state on all the
     * given recipient nodes.
     */
    this.assertRecipientNodesInExpectedState = function(
        nodes, migrationId, tenantId, expectedState, expectedAccessState) {
        nodes.forEach(node => {
            assert(this.isRecipientNodeInExpectedState(
                node, migrationId, tenantId, expectedState, expectedAccessState));
        });
    };

    /**
     * Returns true if the durable and in-memory state for the migration 'migrationId' and
     * 'tenantId' is in the expected state, and false otherwise.
     */
    this.isRecipientNodeInExpectedState = function(
        node, migrationId, tenantId, expectedState, expectedAccessState) {
        const configRecipientsColl =
            this.getRecipientPrimary().getCollection("config.tenantMigrationRecipients");
        if (configRecipientsColl.findOne({_id: migrationId}).state !== expectedState) {
            return false;
        }

        const mtabs =
            assert.commandWorked(node.adminCommand({serverStatus: 1})).tenantMigrationAccessBlocker;
        return (mtabs[tenantId].recipient.state === expectedAccessState);
    };

    function loadDummyData() {
        const numDocs = 20;
        const testData = [];
        for (let i = 0; i < numDocs; ++i) {
            testData.push({_id: i, x: i});
        }
        return testData;
    }

    /**
     * Inserts documents into the specified collection on the donor primary.
     */
    this.insertDonorDB = function(dbName, collName, data = loadDummyData()) {
        jsTestLog(`Inserting data into collection ${collName} of DB ${dbName} on the donor`);
        const db = this.getDonorPrimary().getDB(dbName);
        const coll = db.getCollection(collName);

        assert.commandWorked(coll.insertMany(data, {writeConcern: {w: 'majority'}}));
    };

    /**
     * Verifies that the documents on the recipient primary are correct.
     */
    this.verifyRecipientDB = function(
        tenantId, dbName, collName, migrationCommitted = true, data = loadDummyData()) {
        const shouldMigrate =
            migrationCommitted && TenantMigrationUtil.isNamespaceForTenant(tenantId, dbName);

        jsTestLog(`Verifying that data in collection ${collName} of DB ${dbName} was ${
            (shouldMigrate ? "" : "not")} migrated to the recipient`);

        const db = this.getRecipientPrimary().getDB(dbName);
        const coll = db.getCollection(collName);

        const findRes = coll.find();
        const numDocsFound = findRes.count();

        if (!shouldMigrate) {
            assert.eq(0,
                      numDocsFound,
                      `Find command on recipient collection ${collName} of DB ${
                          dbName} should return 0 docs, instead has count of ${numDocsFound}`);
            return;
        }

        const numDocsExpected = data.length;
        assert.eq(numDocsExpected,
                  numDocsFound,
                  `Find command on recipient collection ${collName} of DB ${dbName} should return ${
                      numDocsExpected} docs, instead has count of ${numDocsFound}`);

        const docsReturned = findRes.sort({_id: 1}).toArray();
        assert(arrayEq(docsReturned, data),
               () => (`${tojson(docsReturned)} is not equal to ${tojson(data)}`));
    };

    /**
     * Crafts a tenant database name.
     */
    this.tenantDB = function(tenantId, dbName) {
        return `${tenantId}_${dbName}`;
    };

    /**
     * Crafts a database name that does not belong to the tenant.
     */
    this.nonTenantDB = function(tenantId, dbName) {
        return `non_${tenantId}_${dbName}`;
    };

    /**
     * Returns the TenantMigrationAccessBlocker associated with the given tenantId on the
     * node.
     */
    this.getTenantMigrationAccessBlocker = function(node, tenantId) {
        return assert.commandWorked(node.adminCommand({serverStatus: 1}))
            .tenantMigrationAccessBlocker[tenantId];
    };

    /**
     * Returns the TenantMigrationStats on the node.
     */
    this.getTenantMigrationStats = function(node) {
        return assert.commandWorked(node.adminCommand({serverStatus: 1})).tenantMigrations;
    };

    /**
     * Awaits the condition when every stats counter reaches the specified count.
     */
    this.awaitTenantMigrationStatsCounts = function(node, {
        currentMigrationsDonating = 0,
        currentMigrationsReceiving = 0,
        totalSuccessfulMigrationsDonated = 0,
        totalSuccessfulMigrationsReceived = 0,
        totalFailedMigrationsDonated = 0,
        totalFailedMigrationsReceived = 0
    }) {
        const check = function(expectedVal, stats, fieldName) {
            if (expectedVal == stats[fieldName]) {
                return true;  // Condition reached, true means the counter reached the target.
            }
            assert.gt(expectedVal,
                      stats[fieldName],
                      `Stat ${fieldName} value ${stats[fieldName]} exceeded the target`);
            return false;
        };
        let stats;
        assert.soon(() => {
            stats = this.getTenantMigrationStats(node);
            if (check(currentMigrationsDonating, stats, "currentMigrationsDonating") &&
                check(currentMigrationsReceiving, stats, "currentMigrationsReceiving") &&
                check(
                    totalSuccessfulMigrationsDonated, stats, "totalSuccessfulMigrationsDonated") &&
                check(totalSuccessfulMigrationsReceived,
                      stats,
                      "totalSuccessfulMigrationsReceived") &&
                check(totalFailedMigrationsDonated, stats, "totalFailedMigrationsDonated") &&
                check(totalFailedMigrationsReceived, stats, "totalFailedMigrationsReceived")) {
                return true;  // Done.
            }
            return false;
        }, `Awaiting for tenant migration stats to reach target, got ${tojson(stats)}`);
    };

    /**
     * Returns the donor ReplSetTest.
     */
    this.getDonorRst = function() {
        return donorRst;
    };

    /**
     * Returns the recipient ReplSetTest.
     */
    this.getRecipientRst = function() {
        return recipientRst;
    };

    /**
     * Returns the donor's primary.
     */
    this.getDonorPrimary = function() {
        return this.getDonorRst().getPrimary();
    };

    /**
     * Returns the recipient's primary.
     */
    this.getRecipientPrimary = function() {
        return this.getRecipientRst().getPrimary();
    };

    /**
     * Returns the recipient's connection string.
     */
    this.getRecipientConnString = function() {
        return this.getRecipientRst().getURL();
    };

    /**
     * Shuts down the donor and recipient sets, only if they were not passed in as parameters.
     * If they were passed in, the test that initialized them should be responsible for shutting
     * them down.
     */
    this.stop = function() {
        if (!donorPassedIn)
            donorRst.stopSet();
        if (!recipientPassedIn)
            recipientRst.stopSet();
    };
}

/**
 * Takes in the response to the donarStartMigration command and asserts the command
 * works and the state is 'committed'.
 */
TenantMigrationTest.assertCommitted = function(stateRes) {
    assert.commandWorked(stateRes);
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kCommitted, tojson(stateRes));
    return stateRes;
};

/**
 * Takes in the response to the donarStartMigration command and asserts the command
 * works and the state is 'aborted', with optional errorCode.
 */
TenantMigrationTest.assertAborted = function(stateRes, errorCode) {
    assert.commandWorked(stateRes);
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kAborted, tojson(stateRes));
    if (errorCode !== undefined) {
        assert.eq(stateRes.abortReason.code, errorCode, tojson(stateRes));
    }
    return stateRes;
};

TenantMigrationTest.DonorState = {
    kCommitted: "committed",
    kAborted: "aborted",
    kDataSync: "data sync",
    kBlocking: "blocking",
    kAbortingIndexBuilds: "aborting index builds",
};

TenantMigrationTest.RecipientState = {
    kStarted: "started",
    kConsistent: "consistent",
    kDone: "done",
};

TenantMigrationTest.State = TenantMigrationTest.DonorState;

TenantMigrationTest.DonorAccessState = {
    kAllow: "allow",
    kBlockWrites: "blockWrites",
    kBlockWritesAndReads: "blockWritesAndReads",
    kReject: "reject",
    kAborted: "aborted",
};

TenantMigrationTest.RecipientAccessState = {
    kReject: "reject",
    kRejectBefore: "rejectBefore"
};

TenantMigrationTest.kConfigDonorsNS = "config.tenantMigrationDonors";
TenantMigrationTest.kConfigRecipientsNS = "config.tenantMigrationRecipients";
