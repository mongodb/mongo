// Test that cluster parameters are correctly enabled/disabled in supported FCV versions, i.e.
// that the set of enabled cluster parameters in a given FCV version is the same as the set of
// the enabled cluster parameters in the matching binary version. This is important to avoid
// issues in multiversion clusters.
// Note: Enabling new feature flags which aren't yet default-enabled will break this test,
// because such feature flags do not have a minimum FCV set and therefore will not be
// disabled when lowering the FCV, causing an incompatibility.
// @tags: [all_feature_flags_incompatible]

import {ReplSetTest} from "jstests/libs/replsettest.js";

// Ignore test parameters because they are not required to be consistent across versions.
const ignoredParams = [
    "testBoolClusterParameter",
    "testIntClusterParameter",
    "testStrClusterParameter",
    "testMinFcvClusterParameter",
    "cwspTestNeedsFeatureFlagBlender",
    "cwspTestNeedsLatestFCV"
];

// The maxAnchorCompactionSize field was added in 7.1.
function cleanFleCompactionOptions(param) {
    if ("maxAnchorCompactionSize" in param) {
        delete param.maxAnchorCompactionSize;
    }
}

// This maps from cluster parameter ID to a cleaning function which can be run on the value of
// that parameter in any valid FCV version to remove any version inconsistencies between FCVs.
// If a cluster parameter is changed between versions, a new entry should be added to this map.
const changedParamsMap = {
    'fleCompactionOptions': cleanFleCompactionOptions
};

// Cluster parameters which changed between versions will not be equal when we compare them later.
// This function ensures that we remove any such version inconsistencies so that we get an equal
// comparison.
function removeVersionInconsistencies(param) {
    if ("_id" in param && param._id in changedParamsMap) {
        changedParamsMap[param._id](param);
    }
}

function runTest(fcvVersion, binaryVersion) {
    // Use getClusterParameter to get a list of all CPs in latest when we downgrade the binary
    // version.
    let newCPs, oldCPs;
    {
        const rst = new ReplSetTest({nodes: [{binVersion: 'latest'}]});
        rst.startSet();
        rst.initiate();
        const conn = rst.getPrimary();
        const db = conn.getDB('admin');

        db.runCommand({setFeatureCompatibilityVersion: fcvVersion, confirm: true});

        newCPs = assert.commandWorked(db.runCommand({getClusterParameter: '*'})).clusterParameters;

        rst.stopSet();
    }

    {
        const rst = new ReplSetTest({nodes: [{binVersion: binaryVersion}]});
        rst.startSet();
        rst.initiate();
        const conn = rst.getPrimary();
        const db = conn.getDB('admin');

        oldCPs = assert.commandWorked(db.runCommand({getClusterParameter: '*'})).clusterParameters;

        rst.stopSet();
    }

    newCPs = newCPs.filter(param => !ignoredParams.includes(param._id));
    oldCPs = oldCPs.filter(param => !ignoredParams.includes(param._id));

    for (let cp of newCPs) {
        removeVersionInconsistencies(cp);
    }
    for (let cp of oldCPs) {
        removeVersionInconsistencies(cp);
    }

    newCPs.sort(bsonWoCompare);
    oldCPs.sort(bsonWoCompare);

    assert.eq(bsonWoCompare(newCPs, oldCPs),
              0,
              "Cluster parameters on new binary not equal to those on old binary\nNew: " +
                  tojson(newCPs) + "\nOld: " + tojson(oldCPs));
}

jsTest.log("Running test on lastLTS...");
runTest(lastLTSFCV, "last-lts");
jsTest.log("Running test on lastContinuous...");
runTest(lastContinuousFCV, "last-continuous");
