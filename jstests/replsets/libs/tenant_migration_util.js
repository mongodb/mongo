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

        const donorRst = new ReplSetTest({rstArgs: donorRstArgs});
        let donorPrimary = donorRst.getPrimary();

        let res;
        assert.soon(() => {
            try {
                res = donorPrimary.adminCommand(cmdObj);

                if (!res.ok) {
                    // If retry is enabled and the command failed with a NotPrimary error, continue
                    // looping.
                    if (retryOnRetryableErrors && ErrorCodes.isNotPrimaryError(res.code)) {
                        donorPrimary = donorRst.getPrimary();
                        return false;
                    }
                    return true;
                }

                return (res.state === "committed" || res.state === "aborted");
            } catch (e) {
                // If the thrown error is network related and we are allowing retryable errors,
                // continue issuing commands.
                if (retryOnRetryableErrors && isNetworkError(e)) {
                    return false;
                }
                throw e;
            }
        });
        return res;
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
        const donorRst = new ReplSetTest({rstArgs: donorRstArgs});
        let donorPrimary = donorRst.getPrimary();

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
    }

    function createRstArgs(donorRst) {
        const donorRstArgs = {
            name: donorRst.name,
            nodeHosts: donorRst.nodes.map(node => `127.0.0.1:${node.port}`),
            nodeOptions: donorRst.nodeOptions,
            keyFile: donorRst.keyFile,
            host: donorRst.host,
            waitForKeys: false,
        };
        return donorRstArgs;
    }

    return {
        runMigrationAsync,
        forgetMigrationAsync,
        createRstArgs,
        isFeatureFlagEnabled,
        getCertificateAndPrivateKey,
        makeX509Options,
        makeMigrationCertificatesForTest,
        makeX509OptionsForTest,
    };
})();
