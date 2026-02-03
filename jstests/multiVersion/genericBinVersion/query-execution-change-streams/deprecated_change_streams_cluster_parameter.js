/**
 * Verifies that deprecated (stubbed) change streams cluster parameters still work
 * after upgrading to latest bin version.
 */

import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

function assertParametersWork(primaryConnection) {
    // The 'changeStreams' parameter was marked as deprecated in v8.3 and does nothing.
    // To keep downwards-compatibility, it is still required that the parameter can be
    // set and requested without error.
    assert.commandWorked(
        primaryConnection.adminCommand({setClusterParameter: {changeStreams: {expireAfterSeconds: 3600}}}),
    );

    const clusterParameters = assert.commandWorked(
        primaryConnection.adminCommand({getClusterParameter: "changeStreams"}),
    ).clusterParameters;
    assert.eq(1, clusterParameters.length);
    assert.eq("changeStreams", clusterParameters[0]._id);
    assert.eq(NumberLong(3600), clusterParameters[0].expireAfterSeconds);
}

testPerformUpgradeSharded({
    setupFn: assertParametersWork,
    whenFullyDowngraded: assertParametersWork,
    whenOnlyConfigIsLatestBinary: assertParametersWork,
    whenSecondariesAndConfigAreLatestBinary: assertParametersWork,
    whenMongosBinaryIsLastLTS: assertParametersWork,
    whenBinariesAreLatestAndFCVIsLastLTS: assertParametersWork,
    whenFullyUpgraded: assertParametersWork,
});
