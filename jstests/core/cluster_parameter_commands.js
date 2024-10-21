// Tests basic set/get facilities for cluster parameters that should be available on all topology
// types.
//
// @tags: [
//   # Runs setClusterParameter, which must be run against mongos in sharded clusters.
//    directly_against_shardsvrs_incompatible,
//   # Runs getClusterParameter which is not allowed with security token.
//    not_allowed_with_signed_security_token,
//   # Transactions aborted upon fcv upgrade or downgrade; cluster parameters use internal txns.
//    uses_transactions,
//   ]

import {
    kTestOnlyClusterParameters,
    testInvalidGetClusterParameter
} from "jstests/libs/cluster_server_parameter_utils.js";

// name => name of cluster parameter to get
// expectedValue => document that should be equal to document describing CP's value, excluding the
// _id
function checkGetClusterParameterMatch(conn, name, expectedValue) {
    const adminDB = conn.getDB('admin');
    const cps =
        assert.commandWorked(adminDB.runCommand({getClusterParameter: name})).clusterParameters;
    // confirm we got the document we were looking for.
    assert.eq(cps.length, 1);
    let actualCp = cps[0];
    assert.eq(actualCp._id, name);
    // confirm the value is expected.
    // remove the id and clusterParameterTime fields
    delete actualCp._id;
    delete actualCp.clusterParameterTime;
    if (bsonWoCompare(actualCp, expectedValue) !== 0) {
        jsTest.log('Server parameter mismatch for parameter ' +
                   '\n' +
                   'Expected: ' + tojson(expectedValue) + '\n' +
                   'Actual: ' + tojson(actualCp));
        return false;
    }
    return true;
}

function runSetClusterParameter(conn, name, value) {
    assert.commandWorked(conn.getDB('admin').runCommand({setClusterParameter: {[name]: value}}));
}

let conn = db.getMongo();

// For each parameter, run setClusterParameter and verify that getClusterParameter
// returns the updated value.

// We need to use assert.soon because, when running against an embedded router,
// we might not see updates right away. TODO SERVER-86543 update this if we guarantee
// strong consistency for getClusterParameter.
for (const [name, data] of Object.entries(kTestOnlyClusterParameters)) {
    if (data.hasOwnProperty('featureFlag')) {
        // Skip testing feature-flag-gated params for now.
        // Difficult to reliably get and check FCV in passthroughs.
        // Feature-flagged cluster parameters are covered in no-passthrough tests.
        continue;
    }
    // Parameters should always start at defaults.
    checkGetClusterParameterMatch(conn, name, data.default);
    runSetClusterParameter(conn, name, data.testValues[0]);
    assert.soon(() => checkGetClusterParameterMatch(conn, name, data.testValues[0]));

    runSetClusterParameter(conn, name, data.testValues[1]);
    assert.soon(() => checkGetClusterParameterMatch(conn, name, data.testValues[1]));

    // Reset everything back to defaults.
    runSetClusterParameter(conn, name, data.default);
    assert.soon(() => checkGetClusterParameterMatch(conn, name, data.default));
}

const tenantId = undefined;
// Assert that invalid uses of getClusterParameter fail.
testInvalidGetClusterParameter(conn, tenantId);

// Assert that setting a nonexistent parameter returns an error.
const adminDB = conn.getDB('admin');
assert.commandFailed(
    adminDB.runCommand({setClusterParameter: {nonexistentParam: {intData: 5}}}, tenantId));

// Assert that running setClusterParameter with a scalar value fails.
assert.commandFailed(
    adminDB.runCommand({setClusterParameter: {testIntClusterParameter: 5}}, tenantId));

// Assert that invalid direct writes to config.clusterParameters fail.
assert.commandFailed(conn.getDB("config").clusterParameters.insert({
    _id: 'testIntClusterParameter',
    foo: 'bar',
    clusterParameterTime: {"$timestamp": {t: 0, i: 0}}
}));

// Assert that the results of getClusterParameter: '*' all have an _id element, and that they are
// consistent with individual gets.
{
    const allParameters =
        assert.commandWorked(adminDB.runCommand({getClusterParameter: '*'})).clusterParameters;
    jsTest.log(allParameters);
    for (const param of allParameters) {
        assert(param.hasOwnProperty("_id"),
               'Entry in {getClusterParameter: "*"} result is missing _id key:\n' + tojson(param));
        const name = param["_id"];
        checkGetClusterParameterMatch(conn, name, param);
    }
}
