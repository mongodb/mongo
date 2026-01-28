/**
 * Tests that we correctly pass the parameter extensionsSignaturePublicKeyPath when runing as part
 * of resmoke. Note, this test is only expected to succeed when it is run as part of a resmoke suite
 * that provides the --loadAllExtensions flag.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
const paramName = "extensionsSignaturePublicKeyPath";
const opts = assert.commandWorked(db.adminCommand({getCmdLineOpts: 1}));
const actualValue = opts["parsed"]["processManagement"][paramName];
const expectedPathSuffix =
    "src/mongo/db/extension/test_examples/test_extensions_signing_keys/test_extensions_signing_public_key.asc";

assert(
    actualValue.endsWith(expectedPathSuffix),
    "Value does not end with expected suffix: " + expectedPathSuffix + " ; got: " + actualValue,
);
