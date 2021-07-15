/**
 * Utilities for testing tenant migrations.
 */
var TenantMigrationUtil = (function() {
    const kExternalKeysNs = "config.external_validation_keys";
    const kCreateRstRetryIntervalMS = 100;

    /**
     * Returns the external keys for the given migration id.
     */
    function getExternalKeys(conn, migrationId) {
        return conn.getCollection(kExternalKeysNs).find({migrationId}).toArray();
    }

    /**
     * Returns whether tenant migration commands are supported.
     */
    function isFeatureFlagEnabled(conn) {
        return assert
            .commandWorked(conn.adminCommand({getParameter: 1, featureFlagTenantMigrations: 1}))
            .featureFlagTenantMigrations.value;
    }

    /**
     * Returns X509 options for ReplSetTest with the given certificate-key file and CA pem file.
     */
    function makeX509Options(certPemFile, caPemFile = "jstests/libs/ca.pem") {
        return {
            // When the global sslMode is preferSSL or requireSSL, the transport layer would do the
            // SSL handshake regardless of the specified sslMode for the connection. So we use a
            // allowTLS to verify that the donor and recipient use SSL to authenticate to each other
            // regardless of the global sslMode.
            tlsMode: "allowTLS",
            tlsCertificateKeyFile: certPemFile,
            tlsCAFile: caPemFile,
            tlsAllowInvalidHostnames: ''
        };
    }

    /**
     * Returns an object containing the certificate and private key extracted from the given
     * pem file.
     */
    function getCertificateAndPrivateKey(pemFile) {
        const lines = cat(pemFile);
        const certificate =
            lines.match(new RegExp("-*BEGIN CERTIFICATE-*\\n(.*\\n)*-*END CERTIFICATE-*\\n"))[0];
        const privateKey =
            lines.match(new RegExp("-*BEGIN PRIVATE KEY-*\\n(.*\\n)*-*END PRIVATE KEY-*\\n"))[0];
        return {certificate, privateKey};
    }

    /**
     * Returns an object containing the donor and recipient ReplSetTest X509 options for tenant
     * migration testing.
     */
    function makeX509OptionsForTest() {
        return {
            donor: makeX509Options("jstests/libs/rs0.pem"),
            recipient: makeX509Options("jstests/libs/rs1.pem")
        };
    }

    /**
     * Returns an object containing the donor and recipient's certificate and private key for
     * tenant migration testing.
     */
    function makeMigrationCertificatesForTest() {
        return {
            donorCertificateForRecipient:
                getCertificateAndPrivateKey("jstests/libs/tenant_migration_donor.pem"),
            recipientCertificateForDonor:
                getCertificateAndPrivateKey("jstests/libs/tenant_migration_recipient.pem")
        };
    }

    /**
     * Takes in the response to the donorStartMigration command and returns true if the state is
     * "committed" or "aborted".
     */
    function isMigrationCompleted(res) {
        return res.state === "committed" || res.state === "aborted";
    }

    /**
     * Runs the donorStartMigration command with the given migration options
     * until the migration commits or aborts, or until the command fails. Returns the last command
     * response.
     *
     * If 'retryOnRetryableErrors' is set, this function will retry if the command fails with a
     * NotPrimary or network error.
     *
     * Only use when it is necessary to run the donorStartMigration command in its own thread. For
     * all other use cases, please consider the runMigration() function in the TenantMigrationTest
     * fixture.
     */
    function runMigrationAsync(migrationOpts, donorRstArgs, retryOnRetryableErrors = false) {
        load("jstests/replsets/libs/tenant_migration_util.js");
        const donorRst = TenantMigrationUtil.createRst(donorRstArgs, retryOnRetryableErrors);

        const migrationCertificates = TenantMigrationUtil.makeMigrationCertificatesForTest();
        const cmdObj = {
            donorStartMigration: 1,
            migrationId: UUID(migrationOpts.migrationIdString),
            recipientConnectionString: migrationOpts.recipientConnString,
            tenantId: migrationOpts.tenantId,
            readPreference: migrationOpts.readPreference || {mode: "primary"},
            donorCertificateForRecipient: migrationOpts.donorCertificateForRecipient ||
                migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationOpts.recipientCertificateForDonor ||
                migrationCertificates.recipientCertificateForDonor
        };

        return TenantMigrationUtil.runTenantMigrationCommand(
            cmdObj, donorRst, retryOnRetryableErrors, TenantMigrationUtil.isMigrationCompleted);
    }

    /**
     * Runs the donorForgetMigration command with the given migrationId and returns the response.
     *
     * If 'retryOnRetryableErrors' is set, this function will retry if the command fails with a
     * NotPrimary or network error.
     *
     * Only use when it is necessary to run the donorForgetMigration command in its own thread. For
     * all other use cases, please consider the forgetMigration() function in the
     * TenantMigrationTest fixture.
     */
    function forgetMigrationAsync(migrationIdString, donorRstArgs, retryOnRetryableErrors = false) {
        load("jstests/replsets/libs/tenant_migration_util.js");
        const donorRst = TenantMigrationUtil.createRst(donorRstArgs, retryOnRetryableErrors);
        const cmdObj = {donorForgetMigration: 1, migrationId: UUID(migrationIdString)};
        return TenantMigrationUtil.runTenantMigrationCommand(
            cmdObj, donorRst, retryOnRetryableErrors);
    }

    /**
     * Runs the donorAbortMigration command with the given migration options and returns the
     * response.
     *
     * If 'retryOnRetryableErrors' is set, this function will retry if the command fails with a
     * NotPrimary or network error.
     *
     * Only use when it is necessary to run the donorAbortMigration command in its own thread. For
     * all other use cases, please consider the tryAbortMigration() function in the
     * TenantMigrationTest fixture.
     */
    function tryAbortMigrationAsync(migrationOpts, donorRstArgs, retryOnRetryableErrors = false) {
        load("jstests/replsets/libs/tenant_migration_util.js");
        const donorRst = TenantMigrationUtil.createRst(donorRstArgs, retryOnRetryableErrors);
        const cmdObj = {
            donorAbortMigration: 1,
            migrationId: UUID(migrationOpts.migrationIdString),
        };
        return TenantMigrationUtil.runTenantMigrationCommand(
            cmdObj, donorRst, retryOnRetryableErrors);
    }

    /**
     * Runs the given tenant migration command against the primary of the given replica set until
     * the command succeeds or fails with a non-retryable error (if 'retryOnRetryableErrors' is
     * true) or until 'shouldStopFunc' returns true (if it is given). Returns the last response.
     */
    function runTenantMigrationCommand(cmdObj, rst, retryOnRetryableErrors, shouldStopFunc) {
        let primary = rst.getPrimary();
        let res;
        assert.soon(() => {
            try {
                res = primary.adminCommand(cmdObj);

                if (!res.ok) {
                    // If retry is enabled and the command failed with a NotPrimary error, continue
                    // looping.
                    if (retryOnRetryableErrors && ErrorCodes.isNotPrimaryError(res.code)) {
                        primary = rst.getPrimary();
                        return false;
                    }
                    return true;
                }

                if (shouldStopFunc) {
                    return shouldStopFunc(res);
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
    }

    function createRstArgs(rst) {
        const rstArgs = {
            name: rst.name,
            nodeHosts: rst.nodes.map(node => `127.0.0.1:${node.port}`),
            nodeOptions: rst.nodeOptions,
            keyFile: rst.keyFile,
            host: rst.host,
            waitForKeys: false,
        };
        return rstArgs;
    }

    /**
     * Returns a new ReplSetTest created based on the given 'rstArgs'. If 'retryOnRetryableErrors'
     * is true, retries on retryable errors (e.g. errors caused by shutdown).
     */
    function createRst(rstArgs, retryOnRetryableErrors) {
        while (true) {
            try {
                return new ReplSetTest({rstArgs: rstArgs});
            } catch (e) {
                if (retryOnRetryableErrors && isNetworkError(e)) {
                    jsTest.log(`Failed to create ReplSetTest for ${
                        rstArgs.name} inside tenant migration thread: ${tojson(e)}. Retrying in ${
                        kCreateRstRetryIntervalMS}ms.`);
                    sleep(kCreateRstRetryIntervalMS);
                    continue;
                }
                throw e;
            }
        }
    }

    /**
     * Returns the TenantMigrationAccessBlocker serverStatus output for the migration for the given
     * tenant if there one.
     */
    function getTenantMigrationAccessBlocker(node, tenantId) {
        const mtabs =
            assert.commandWorked(node.adminCommand({serverStatus: 1})).tenantMigrationAccessBlocker;
        if (!mtabs) {
            return null;
        }
        return mtabs[tenantId];
    }

    /**
     * Returns the number of reads on the given donor node that were blocked due to tenant migration
     * for the given tenant.
     */
    function getNumBlockedReads(donorNode, tenantId) {
        const mtab = getTenantMigrationAccessBlocker(donorNode, tenantId);
        if (!mtab) {
            return 0;
        }
        return mtab.donor.numBlockedReads;
    }

    /**
     * Returns the number of writes on the given donor node that were blocked due to tenant
     * migration for the given tenant.
     */
    function getNumBlockedWrites(donorNode, tenantId) {
        const mtab = getTenantMigrationAccessBlocker(donorNode, tenantId);
        if (!mtab) {
            return 0;
        }
        return mtab.donor.numBlockedWrites;
    }

    /**
     * Determines if a database name belongs to the given tenant.
     */
    function isNamespaceForTenant(tenantId, dbName) {
        return dbName.startsWith(`${tenantId}_`);
    }

    /**
     * Compares the hashes for DBs that belong to the specified tenant between the donor and
     * recipient primaries.
     */
    function checkTenantDBHashes(donorRst,
                                 recipientRst,
                                 tenantId,
                                 excludedDBs = [],
                                 msgPrefix = 'checkTenantDBHashes',
                                 ignoreUUIDs = false) {
        // Always skip db hash checks for the config, admin, and local database.
        excludedDBs = [...excludedDBs, "config", "admin", "local"];

        while (true) {
            try {
                const donorPrimaryConn = donorRst.getPrimary();
                const recipientPrimaryConn = recipientRst.getPrimary();

                // Allows listCollections and listIndexes on donor after migration for consistency
                // checks.
                const donorAllowsReadsAfterMigration =
                    assert
                        .commandWorked(donorPrimaryConn.adminCommand({
                            getParameter: 1,
                            "failpoint.tenantMigrationDonorAllowsNonTimestampedReads": 1
                        }))["failpoint.tenantMigrationDonorAllowsNonTimestampedReads"]
                        .mode;
                // Only turn on the failpoint if it is not already.
                if (!donorAllowsReadsAfterMigration) {
                    assert.commandWorked(donorPrimaryConn.adminCommand({
                        configureFailPoint: "tenantMigrationDonorAllowsNonTimestampedReads",
                        mode: "alwaysOn"
                    }));
                }

                // Filter out all dbs that don't belong to the tenant.
                let combinedDBNames =
                    [...donorPrimaryConn.getDBNames(), ...recipientPrimaryConn.getDBNames()];
                combinedDBNames =
                    combinedDBNames.filter(dbName => (isNamespaceForTenant(tenantId, dbName) &&
                                                      !excludedDBs.includes(dbName)));
                combinedDBNames = new Set(combinedDBNames);

                for (const dbName of combinedDBNames) {
                    // Pass in an empty array for the secondaries, since we only wish to compare
                    // the DB hashes between the donor and recipient primary in this test.
                    const donorDBHash =
                        assert.commandWorked(donorRst.getHashes(dbName, []).primary);
                    const recipientDBHash =
                        assert.commandWorked(recipientRst.getHashes(dbName, []).primary);

                    const donorCollections = Object.keys(donorDBHash.collections);
                    const donorCollInfos = new CollInfos(donorPrimaryConn, 'donorPrimary', dbName);
                    donorCollInfos.filter(donorCollections);

                    const recipientCollections = Object.keys(recipientDBHash.collections);
                    const recipientCollInfos =
                        new CollInfos(recipientPrimaryConn, 'recipientPrimary', dbName);
                    recipientCollInfos.filter(recipientCollections);

                    print(`checking db hash between donor: ${donorPrimaryConn} and recipient: ${
                        recipientPrimaryConn}`);

                    const collectionPrinted = new Set();
                    const success = DataConsistencyChecker.checkDBHash(donorDBHash,
                                                                       donorCollInfos,
                                                                       recipientDBHash,
                                                                       recipientCollInfos,
                                                                       msgPrefix,
                                                                       ignoreUUIDs,
                                                                       true, /* syncingHasIndexes */
                                                                       collectionPrinted);
                    if (!success) {
                        print(`checkTenantDBHashes dumping donor and recipient primary oplogs`);
                        donorRst.dumpOplog(donorPrimaryConn, {}, 100);
                        recipientRst.dumpOplog(recipientPrimaryConn, {}, 100);
                    }
                    assert(success, 'dbhash mismatch between donor and recipient primaries');
                }

                // Reset failpoint on the donor after consistency checks if it wasn't enabled
                // before.
                if (!donorAllowsReadsAfterMigration) {
                    // We unset the failpoint for every node in case there was a failover at some
                    // point before this.
                    donorRst.nodes.forEach(node => {
                        assert.commandWorked(node.adminCommand({
                            configureFailPoint: "tenantMigrationDonorAllowsNonTimestampedReads",
                            mode: "off"
                        }));
                    });
                }

                break;
            } catch (e) {
                if (!checkIfRetriableErrorForTenantDbHashCheck(e)) {
                    throw e;
                } else {
                    print(`Got error: ${tojson(e)}. Failover occurred during tenant dbhash check, retrying 
                        tenant dbhash check.`);
                }
            }
        }
    }

    /**
     * Creates a role for tenant migration donor if it doesn't exist.
     */
    function createTenantMigrationDonorRoleIfNotExist(rst) {
        const adminDB = rst.getPrimary().getDB("admin");

        if (roleExists(adminDB, "tenantMigrationDonorRole")) {
            return;
        }

        assert.commandWorked(adminDB.runCommand({
            createRole: "tenantMigrationDonorRole",
            privileges: [
                {resource: {cluster: true}, actions: ["runTenantMigration"]},
                {resource: {db: "admin", collection: "system.keys"}, actions: ["find"]}
            ],
            roles: []
        }));
    }

    /**
     * Checks if an error gotten while doing a tenant dbhash check is retriable.
     */
    function checkIfRetriableErrorForTenantDbHashCheck(error) {
        // Due to the shell not propagating error codes correctly, if we get any of the following
        // error messages, we can retry the operation.
        const retryableErrorMessages = [
            "The server is in quiesce mode and will shut down",
            "can't connect to new replica set primary"
        ];

        return ErrorCodes.isRetriableError(error.code) || ErrorCodes.isNetworkError(error.code) ||
            // The following shell helper methods check if the error message contains some
            // notion of retriability. This is in case the error does not contain an error code.
            isRetryableError(error) || isNetworkError(error) ||
            // If there's a failover while we're running a dbhash check, the elected secondary might
            // not have set the tenantMigrationDonorAllowsNonTimestampedReads failpoint, which means
            // that the listCollections command run when we call CollInfos would throw a
            // TenantMigrationCommitted error.
            ErrorCodes.isTenantMigrationError(error.code) ||
            // If there's a failover as we're creating a ReplSetTest from either the donor or
            // recipient URLs, it's possible to get back a NotYetInitialized error, so we want to
            // retry creating the ReplSetTest.
            error.code == ErrorCodes.NotYetInitialized ||
            // TODO (SERVER-54026): Remove check for error message once the shell correctly
            // propagates the error code.
            retryableErrorMessages.some(msg => error.message.includes(msg));
    }

    /**
     * Creates a role for tenant migration recipient if it doesn't exist.
     */
    function createTenantMigrationRecipientRoleIfNotExist(rst) {
        const adminDB = rst.getPrimary().getDB("admin");

        if (roleExists(adminDB, "tenantMigrationRecipientRole")) {
            return;
        }

        assert.commandWorked(adminDB.runCommand({
            createRole: "tenantMigrationRecipientRole",
            privileges: [
                {
                    resource: {cluster: true},
                    actions: ["listDatabases", "useUUID", "advanceClusterTime"]
                },
                {resource: {db: "", collection: ""}, actions: ["listCollections"]},
                {
                    resource: {anyResource: true},
                    actions: ["dbStats", "collStats", "find", "listIndexes"]
                }
            ],
            roles: []
        }));
    }

    /**
     * Returns true if the given database role already exists.
     */
    function roleExists(db, roleName) {
        const roles = db.getRoles({rolesInfo: 1, showPrivileges: false, showBuiltinRoles: false});
        const fullRoleName = `${db.getName()}.${roleName}`;
        for (let role of roles) {
            if (role._id == fullRoleName) {
                return true;
            }
        }
        return false;
    }

    return {
        kExternalKeysNs,
        getExternalKeys,
        runMigrationAsync,
        forgetMigrationAsync,
        tryAbortMigrationAsync,
        createRstArgs,
        createRst,
        runTenantMigrationCommand,
        isFeatureFlagEnabled,
        getCertificateAndPrivateKey,
        makeX509Options,
        makeMigrationCertificatesForTest,
        makeX509OptionsForTest,
        isMigrationCompleted,
        getTenantMigrationAccessBlocker,
        getNumBlockedReads,
        getNumBlockedWrites,
        isNamespaceForTenant,
        checkTenantDBHashes,
        createTenantMigrationDonorRoleIfNotExist,
        createTenantMigrationRecipientRoleIfNotExist,
        roleExists,
        checkIfRetriableErrorForTenantDbHashCheck
    };
})();
