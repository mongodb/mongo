/**
 * Wrapper around ReplSetTest for testing tenant migration behavior.
 */

"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/replsets/rslib.js");
load('jstests/libs/fail_point_util.js');

/**
 * This fixture allows the user to optionally pass in a custom ReplSetTest for the donor and
 * recipient replica sets, to be used for the test.
 *
 * If the caller does not provide their own replica set, a two node replset will be initialized
 * instead, with all nodes running the latest version.
 *
 * @param {string} [name] the name of the replica sets
 * @param {Object} [donorRst] the ReplSetTest instance to adopt for the donor
 * @param {Object} [recipientRst] the ReplSetTest instance to adopt for the recipient
 */
function TenantMigrationTest(
    {name = "TenantMigrationTest", enableRecipientTesting = true, donorRst, recipientRst}) {
    const donorPassedIn = (donorRst !== undefined);
    const recipientPassedIn = (recipientRst !== undefined);

    donorRst = donorPassedIn ? donorRst : performSetUp(true /* isDonor */);
    recipientRst = recipientPassedIn ? recipientRst : performSetUp(false /* isDonor */);

    donorRst.getPrimary();
    donorRst.awaitReplication();

    recipientRst.getPrimary();
    recipientRst.awaitReplication();

    /**
     * Creates a ReplSetTest instance. The repl set will have 2 nodes.
     */
    function performSetUp(isDonor) {
        let setParameterOpts = {};
        if (TestData.logComponentVerbosity) {
            setParameterOpts["logComponentVerbosity"] =
                tojsononeline(TestData.logComponentVerbosity);
        }

        if (!(isDonor || enableRecipientTesting)) {
            setParameterOpts["failpoint.returnResponseOkForRecipientSyncDataCmd"] =
                tojson({mode: 'alwaysOn'});
        }

        let nodeOptions = {};
        nodeOptions["setParameter"] = setParameterOpts;

        const rstName = `${name}_${(isDonor ? "donor" : "recipient")}`;
        const rst = new ReplSetTest({name: rstName, nodes: 2, nodeOptions});
        rst.startSet();
        rst.initiateWithHighElectionTimeout();

        return rst;
    }

    /**
     * Returns whether tenant migration commands are supported.
     */
    this.isFeatureFlagEnabled = function() {
        const donorPrimary = this.getDonorPrimary();
        const recipientPrimary = this.getRecipientPrimary();

        function supportsTenantMigrations(conn) {
            return assert
                .commandWorked(conn.adminCommand({getParameter: 1, featureFlagTenantMigrations: 1}))
                .featureFlagTenantMigrations.value;
        }
        const retVal =
            (supportsTenantMigrations(donorPrimary) && supportsTenantMigrations(recipientPrimary));
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
    this.runMigration = function(migrationOpts, retryOnRetryableErrors = false) {
        const res = this.startMigration(migrationOpts, retryOnRetryableErrors);
        if (!res.ok) {
            return res;
        }

        return this.waitForMigrationToComplete(migrationOpts, retryOnRetryableErrors);
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
    },
                                           waitForMigrationToComplete,
                                           retryOnRetryableErrors) {
        const cmdObj = {
            donorStartMigration: 1,
            tenantId,
            migrationId: UUID(migrationIdString),
            recipientConnectionString,
            readPreference,
        };

        let donorPrimary = this.getDonorPrimary();
        let stateRes;

        assert.soon(() => {
            try {
                stateRes = donorPrimary.adminCommand(cmdObj);

                if (!stateRes.ok) {
                    // If retry is enabled and the command failed with a NotPrimary error, continue
                    // looping.
                    if (retryOnRetryableErrors && ErrorCodes.isNotPrimaryError(stateRes.code)) {
                        donorPrimary = donorRst.getPrimary();
                        return false;
                    }
                    return true;
                }

                // The command has been successfully executed. If we don't need to wait for the
                // migration to complete, exit the loop.
                if (!waitForMigrationToComplete) {
                    return true;
                }

                return (stateRes.state === TenantMigrationTest.State.kCommitted ||
                        stateRes.state === TenantMigrationTest.State.kAborted);
            } catch (e) {
                // If the thrown error is network related and we are allowing retryable errors,
                // continue issuing commands.
                if (retryOnRetryableErrors && isNetworkError(e)) {
                    return false;
                }
                throw e;
            }
        });
        return stateRes;
    };

    /**
     * Runs the donorForgetMigration command with the given migrationId and returns the response.
     *
     * If 'retryOnRetryableErrors' is set, this function will retry if the command fails with a
     * NotPrimary or network error.
     */
    this.forgetMigration = function(migrationIdString, retryOnRetryableErrors = false) {
        let donorPrimary = this.getDonorPrimary();

        let res;

        assert.soon(() => {
            try {
                res = donorPrimary.adminCommand(
                    {donorForgetMigration: 1, migrationId: UUID(migrationIdString)});

                if (!res.ok) {
                    // If retry is enabled and the command failed with a NotPrimary error, continue
                    // looping.
                    if (retryOnRetryableErrors && ErrorCodes.isNotPrimaryError(res.code)) {
                        donorPrimary = donorRst.getPrimary();
                        return false;
                    }
                }

                return true;
            } catch (e) {
                if (retryOnRetryableErrors && isNetworkError(e)) {
                    return false;
                }
                throw e;
            }
        });
        return res;
    };

    /**
     * Asserts that durable and in-memory state for the migration 'migrationId' and 'tenantId' is
     * eventually deleted from the given nodes.
     */
    this.waitForMigrationGarbageCollection = function(nodes, migrationId, tenantId) {
        nodes.forEach(node => {
            const configDonorsColl = node.getCollection("config.tenantMigrationDonors");
            assert.soon(() => 0 === configDonorsColl.count({_id: migrationId}));

            assert.soon(() => 0 ===
                            assert.commandWorked(node.adminCommand({serverStatus: 1}))
                                .repl.primaryOnlyServices.TenantMigrationDonorService);

            let mtabs;
            assert.soon(() => {
                mtabs = assert.commandWorked(node.adminCommand({serverStatus: 1}))
                            .tenantMigrationAccessBlocker;
                return !mtabs || !mtabs[tenantId];
            }, tojson(mtabs));
        });
    };

    /**
     * Asserts that the migration 'migrationId' and 'tenantId' eventually goes to the
     * expected state on all the given nodes.
     */
    this.waitForNodesToReachState = function(nodes, migrationId, tenantId, expectedState) {
        nodes.forEach(node => {
            assert.soon(() =>
                            this.isNodeInExpectedState(node, migrationId, tenantId, expectedState));
        });
    };

    /**
     * Asserts that the migration 'migrationId' and 'tenantId' is in the expected state on all the
     * given nodes.
     */
    this.assertNodesInExpectedState = function(nodes, migrationId, tenantId, expectedState) {
        nodes.forEach(node => {
            assert(this.isNodeInExpectedState(node, migrationId, tenantId, expectedState));
        });
    };

    /**
     * Returns true if the durable and in-memory state for the migration 'migrationId' and
     * 'tenantId' is in the expected state, and false otherwise.
     */
    this.isNodeInExpectedState = function(node, migrationId, tenantId, expectedState) {
        const configDonorsColl =
            this.getDonorPrimary().getCollection("config.tenantMigrationDonors");
        if (configDonorsColl.findOne({_id: migrationId}).state !== expectedState) {
            return false;
        }

        const expectedAccessState = (expectedState === TenantMigrationTest.State.kCommitted)
            ? TenantMigrationTest.AccessState.kReject
            : TenantMigrationTest.AccessState.kAborted;
        const mtabs =
            assert.commandWorked(node.adminCommand({serverStatus: 1})).tenantMigrationAccessBlocker;
        return (mtabs[tenantId].state === expectedAccessState);
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

        assert.commandWorked(coll.insertMany(data));
    };

    /**
     * Verifies that the documents on the recipient primary are correct.
     */
    this.verifyReceipientDB = function(tenantId, dbName, collName, data = loadDummyData()) {
        const shouldMigrate = this.isNamespaceForTenant(tenantId, dbName);

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
     * Determines if a database name belongs to the given tenant.
     */
    this.isNamespaceForTenant = function(tenantId, dbName) {
        return dbName.startsWith(`${tenantId}_`);
    };

    /**
     * Returns the TenantMigrationAccessBlocker associated with given the tenantId on the
     * node.
     */
    this.getTenantMigrationAccessBlocker = function(node, tenantId) {
        return assert.commandWorked(node.adminCommand({serverStatus: 1}))
            .tenantMigrationAccessBlocker[tenantId];
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

TenantMigrationTest.State = {
    kCommitted: "committed",
    kAborted: "aborted",
    kDataSync: "data sync",
    kBlocking: "blocking",
};

TenantMigrationTest.AccessState = {
    kAllow: "allow",
    kBlockWrites: "blockWrites",
    kBlockWritesAndReads: "blockWritesAndReads",
    kReject: "reject",
    kAborted: "aborted",
};

TenantMigrationTest.kConfigDonorsNS = "config.tenantMigrationDonors";
