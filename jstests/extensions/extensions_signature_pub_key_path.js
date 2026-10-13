/**
 * Tests that we correctly pass the parameter extensionsSignaturePublicKeyPath when runing as part
 * of resmoke. Note, this test is only expected to succeed when it is run as part of a resmoke suite
 * that provides the --loadAllExtensions flag.
 *
 * Some suite configurations (i.e extensions-secure/release) deliberately skip injecting the
 * extensionsSignaturePublicKeyPath. Instead, validation is performed against the embedded
 * production key, which test extensions (signed with the test key) can never pass, so resmoke is
 * invoked with --skipExtensionsSignatureVerification and does not inject the test key path.
 * Only skip this check for that explicit configuration which is surfaced as
 * TestData.skipExtensionsSignatureVerification.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
if (TestData.skipExtensionsSignatureVerification) {
    jsTest.log.info(
        "Resmoke skipped extension signature verification (secure-mode build); " +
            "extensionsSignaturePublicKeyPath is compiled out, skipping the check.",
    );
} else {
    const paramName = "extensionsSignaturePublicKeyPath";
    const opts = assert.commandWorked(db.adminCommand({getCmdLineOpts: 1}));
    const actualValue = opts["parsed"]["processManagement"][paramName];
    const expectedPathSuffix = "test_extensions_signing_public_key.asc";

    assert(actualValue !== undefined, "extensionsSignaturePublicKeyPath is not set", {
        cmdLineOpts: opts["parsed"]["processManagement"],
    });
    assert(actualValue.endsWith(expectedPathSuffix), "Value does not end with expected suffix", {
        expectedPathSuffix,
        actualValue,
    });
}
