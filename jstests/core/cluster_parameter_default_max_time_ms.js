/**
 * Tests that 'defaultMaxTimeMS' can be get and set correctly using the cluster parameter commands.
 *
 * @tags: [
 *   # Runs getClusterParameter which is not allowed with security token.
 *   not_allowed_with_signed_security_token,
 *   # Transactions aborted upon fcv upgrade or downgrade; cluster parameters use internal txns.
 *   uses_transactions,
 *   featureFlagDefaultReadMaxTimeMS,
 *   # TODO (SERVER-88924): Re-enable the test.
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

const adminDB = db.getSiblingDB("admin");

function checkDefaultReadMaxTimeMSVal(expectedVal) {
    const res = assert.commandWorked(adminDB.runCommand({getClusterParameter: "defaultMaxTimeMS"}));
    assert.eq(res.clusterParameters.length, 1);
    assert.eq(res.clusterParameters[0]._id, "defaultMaxTimeMS");
    assert.eq(res.clusterParameters[0].readOperations, expectedVal);
}

// Checks the default value.
checkDefaultReadMaxTimeMSVal(0);

assert.commandFailedWithCode(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: "str"}}}),
    ErrorCodes.TypeMismatch);

assert.commandFailedWithCode(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: -1}}}),
    ErrorCodes.BadValue);

// Fails when setting an unknown subfield.
assert.commandFailedWithCode(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {unknownField: 42}}}), 40415);

// Sets to a valid user-defined value.
assert.commandWorked(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 42}}}));
checkDefaultReadMaxTimeMSVal(42);

// Unsets the cluster parameter.
assert.commandWorked(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 0}}}));
checkDefaultReadMaxTimeMSVal(0);
