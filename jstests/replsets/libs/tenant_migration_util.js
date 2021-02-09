/**
 * Utilities for testing tenant migrations.
 */
var TenantMigrationUtil = (function() {
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
                getCertificateAndPrivateKey("jstests/libs/rs0_tenant_migration.pem"),
            recipientCertificateForDonor:
                getCertificateAndPrivateKey("jstests/libs/rs1_tenant_migration.pem")
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
        const donorRst = new ReplSetTest({rstArgs: donorRstArgs});

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
        const donorRst = new ReplSetTest({rstArgs: donorRstArgs});
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
        const donorRst = new ReplSetTest({rstArgs: donorRstArgs});
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
        return mtab.numBlockedReads;
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
        return mtab.numBlockedWrites;
    }

    return {
        runMigrationAsync,
        forgetMigrationAsync,
        tryAbortMigrationAsync,
        createRstArgs,
        runTenantMigrationCommand,
        isFeatureFlagEnabled,
        getCertificateAndPrivateKey,
        makeX509Options,
        makeMigrationCertificatesForTest,
        makeX509OptionsForTest,
        isMigrationCompleted,
        getTenantMigrationAccessBlocker,
        getNumBlockedReads,
        getNumBlockedWrites
    };
})();
