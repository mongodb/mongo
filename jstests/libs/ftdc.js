/**
 * Utility test functions for FTDC
 */

import {isClusterNode, isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

export function getParameter(adminDb, field) {
    var q = {getParameter: 1};
    q[field] = 1;

    var ret = adminDb.runCommand(q);
    return ret[field];
}

export function setParameter(adminDb, obj) {
    let o = Object.extend({setParameter: 1}, obj, true);
    return adminDb.runCommand(Object.extend({setParameter: 1}, obj));
}

/**
 * Returns whether the FTDC file format should follow the new format or not.
 */
export function hasMultiserviceFTDCSchema(adminDb) {
    return FeatureFlagUtil.isPresentAndEnabled(adminDb, "MultiServiceLogAndFTDCFormat") &&
        (isMongos(adminDb) || isClusterNode(adminDb));
}

/**
 * `has(obj, "foo", "bar", "baz")` returns whether `obj.foo.bar.baz` exists.
 */
function has(object, ...properties) {
    if (properties.length === 0) {
        return true;
    }
    const [prop, ...rest] = properties;
    return object.hasOwnProperty(prop) && has(object[prop], ...rest);
}

/**
 * Returns an array of predicate functions used by `verifyGetDiagnosticData` to
 * determine whether the response returned for "getDiagnosticData" is as
 * expected.
 */
function getCriteriaForGetDiagnosticData({data, adminDb, assumeMultiserviceSchema}) {
    let criteria = [];

    criteria.push(() => has(data, "start"));

    if (hasMultiserviceFTDCSchema(adminDb) || assumeMultiserviceSchema ||
        TestData.testingReplicaSetEndpoint) {
        criteria.push(() => has(data, "shard", "serverStatus") ||
                          has(data, "router", "connPoolStats"));
    } else {
        criteria.push(() => has(data, "serverStatus"));
    }

    criteria.push(() => has(data, "end"));

    return criteria;
}

/**
 * Verify that getDiagnosticData is working correctly.
 */
export function verifyGetDiagnosticData(adminDb, logData = true, assumeMultiserviceSchema = false) {
    const maxAttempts = 60;
    const retryMillis = 500;
    // We need to retry a few times in case we're running this test immediately
    // after mongod is started. FTDC may not have run yet, or some collectors
    // might have timed out initially and so be missing from the response.
    for (let attempt = 1;; ++attempt) {
        const result = adminDb.runCommand("getDiagnosticData");
        assert.commandWorked(result);
        const data = result.data;
        const criteria = getCriteriaForGetDiagnosticData({data, adminDb, assumeMultiserviceSchema});
        // results :: {[some predicate]: bool result, ...}
        const results = criteria.reduce((results, predicate) => {
            results[predicate.toString()] = predicate();
            return results;
        }, {});
        if (Object.values(results).indexOf(false) === -1) {
            // all predicates are satisfied
            if (logData) {
                jsTestLog("getDiagnosticData response met all criteria: " +
                          tojson({criteria: results}));
            }
            return data;
        }

        assert(attempt < maxAttempts,
               `getDiagnosticData response failed to satisfy criteria after ${
                   maxAttempts} attempts: ` +
                   tojson({criteria: results, data}));

        jsTestLog(
            `getDiagnosticData response did not satisfy one or more criteria. Trying again in ${
                retryMillis} milliseconds (attempt ${attempt}/${maxAttempts}). Criteria: ` +
            tojson(results));
        sleep(retryMillis);
    }
}

/**
 * Validate all the common FTDC parameters are set correctly and can be manipulated.
 */
export function verifyCommonFTDCParameters(adminDb, isEnabled) {
    // Are we running against MongoS?
    var isMongos = ("isdbgrid" == adminDb.runCommand("ismaster").msg);

    // Check the defaults are correct
    //
    function getparam(field) {
        return getParameter(adminDb, field);
    }

    // Verify the defaults are as we documented them
    assert.eq(getparam("diagnosticDataCollectionEnabled"), isEnabled);
    assert.eq(getparam("diagnosticDataCollectionPeriodMillis"), 1000);

    const diagnosticDataCollectionDirectorySizeMB =
        getparam("diagnosticDataCollectionDirectorySizeMB");

    const isShardedCluster = adminDb.system.version.findOne({_id: "shardIdentity"});
    if (isShardedCluster &&
        FeatureFlagUtil.isPresentAndEnabled(adminDb, "MultiServiceLogAndFTDCFormat") && !isMongos) {
        assert.eq(diagnosticDataCollectionDirectorySizeMB, 500);
    } else {
        assert.eq(diagnosticDataCollectionDirectorySizeMB, 250);
    }

    assert.eq(getparam("diagnosticDataCollectionFileSizeMB"), 10);
    assert.eq(getparam("diagnosticDataCollectionSamplesPerChunk"), 300);
    assert.eq(getparam("diagnosticDataCollectionSamplesPerInterimUpdate"), 10);

    function setparam(obj) {
        return setParameter(adminDb, obj);
    }

    if (!isMongos) {
        // The MongoS specific behavior for diagnosticDataCollectionEnabled is tested in
        // ftdc_setdirectory.js.
        assert.commandWorked(setparam({"diagnosticDataCollectionEnabled": 1}));
    }
    assert.commandWorked(setparam({"diagnosticDataCollectionPeriodMillis": 100}));
    assert.commandWorked(setparam({"diagnosticDataCollectionDirectorySizeMB": 10}));
    assert.commandWorked(setparam({"diagnosticDataCollectionFileSizeMB": 1}));
    assert.commandWorked(setparam({"diagnosticDataCollectionSamplesPerChunk": 2}));
    assert.commandWorked(setparam({"diagnosticDataCollectionSamplesPerInterimUpdate": 2}));

    // Negative tests - set values below minimums
    assert.commandFailed(setparam({"diagnosticDataCollectionPeriodMillis": 1}));
    assert.commandFailed(setparam({"diagnosticDataCollectionDirectorySizeMB": 1}));
    assert.commandFailed(setparam({"diagnosticDataCollectionSamplesPerChunk": 1}));
    assert.commandFailed(setparam({"diagnosticDataCollectionSamplesPerInterimUpdate": 1}));

    // Negative test - set file size bigger then directory size
    assert.commandWorked(setparam({"diagnosticDataCollectionDirectorySizeMB": 10}));
    assert.commandFailed(setparam({"diagnosticDataCollectionFileSizeMB": 100}));

    // Negative test - set directory size less then file size
    assert.commandWorked(setparam({"diagnosticDataCollectionDirectorySizeMB": 100}));
    assert.commandWorked(setparam({"diagnosticDataCollectionFileSizeMB": 50}));
    assert.commandFailed(setparam({"diagnosticDataCollectionDirectorySizeMB": 10}));

    // Reset
    assert.commandWorked(setparam({"diagnosticDataCollectionFileSizeMB": 10}));
    assert.commandWorked(setparam(
        {"diagnosticDataCollectionDirectorySizeMB": diagnosticDataCollectionDirectorySizeMB}));
    assert.commandWorked(setparam({"diagnosticDataCollectionPeriodMillis": 1000}));
    assert.commandWorked(setparam({"diagnosticDataCollectionSamplesPerChunk": 300}));
    assert.commandWorked(setparam({"diagnosticDataCollectionSamplesPerInterimUpdate": 10}));
}

export function waitFailedToStart(pid, exitCode) {
    assert.soon(function() {
        return !checkProgram(pid).alive;
    }, `Failed to wait for ${pid} to die`, 30 * 1000);

    assert.eq(exitCode,
              checkProgram(pid).exitCode,
              `Failed to wait for ${pid} to die with exit code ${exitCode}`);
}
