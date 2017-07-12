/**
 * Utility test functions for FTDC
 */
'use strict';

/**
 * Verify that getDiagnosticData is working correctly.
 */
function verifyGetDiagnosticData(adminDb) {
    // We need to retry a few times if run this test immediately after mongod is started as FTDC may
    // not have run yet.
    var foundGoodDocument = false;

    for (var i = 0; i < 60 && foundGoodDocument == false; ++i) {
        var result = adminDb.runCommand("getDiagnosticData");
        assert.commandWorked(result);

        var data = result.data;

        if (!data.hasOwnProperty("start")) {
            // Wait a little longer for FTDC to start
            jsTestLog("Running getDiagnosticData: " + tojson(result));

            sleep(500);
        } else {
            // Check for a few common properties to ensure we got data
            assert(data.hasOwnProperty("serverStatus"),
                   "does not have 'serverStatus' in '" + tojson(data) + "'");
            assert(data.hasOwnProperty("end"), "does not have 'end' in '" + tojson(data) + "'");
            foundGoodDocument = true;

            jsTestLog("Got good getDiagnosticData: " + tojson(result));
        }
    }

    assert(foundGoodDocument,
           "getDiagnosticData failed to return a non-empty command, is FTDC running?");
}

/**
 * Validate all the common FTDC parameters are set correctly and can be manipulated.
 */
function verifyCommonFTDCParameters(adminDb, isEnabled) {
    // Are we running against MongoS?
    var isMongos = ("isdbgrid" == adminDb.runCommand("ismaster").msg);

    // Check the defaults are correct
    //
    function getparam(field) {
        var q = {getParameter: 1};
        q[field] = 1;

        var ret = adminDb.runCommand(q);
        return ret[field];
    }

    // Verify the defaults are as we documented them
    assert.eq(getparam("diagnosticDataCollectionEnabled"), isEnabled);
    assert.eq(getparam("diagnosticDataCollectionPeriodMillis"), 1000);
    assert.eq(getparam("diagnosticDataCollectionDirectorySizeMB"), 200);
    assert.eq(getparam("diagnosticDataCollectionFileSizeMB"), 10);
    assert.eq(getparam("diagnosticDataCollectionSamplesPerChunk"), 300);
    assert.eq(getparam("diagnosticDataCollectionSamplesPerInterimUpdate"), 10);

    function setparam(obj) {
        var ret = adminDb.runCommand(Object.extend({setParameter: 1}, obj));
        return ret;
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
    assert.commandWorked(setparam({"diagnosticDataCollectionDirectorySizeMB": 200}));
    assert.commandWorked(setparam({"diagnosticDataCollectionPeriodMillis": 1000}));
    assert.commandWorked(setparam({"diagnosticDataCollectionSamplesPerChunk": 300}));
    assert.commandWorked(setparam({"diagnosticDataCollectionSamplesPerInterimUpdate": 10}));
}