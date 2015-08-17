// FTDC test cases
//
(function () {
    'use strict';
    var admin = db.getSiblingDB( "admin" );

    // Check the defaults are correct
    //
    function getparam(field) {
        var q = { getParameter : 1 };
        q[field] = 1;

        var ret = admin.runCommand( q );
        return ret[field];
    }

    // Verify the defaults are as we documented them
    assert.eq(getparam("diagnosticDataCollectionEnabled"), true);
    assert.eq(getparam("diagnosticDataCollectionPeriodMillis"), 1000);
    assert.eq(getparam("diagnosticDataCollectionDirectorySizeMb"), 100);
    assert.eq(getparam("diagnosticDataCollectionFileSizeMb"), 10);
    assert.eq(getparam("diagnosticDataCollectionSamplesPerChunk"), 300);
    assert.eq(getparam("diagnosticDataCollectionSamplesPerInterimUpdate"), 10);

    function setparam(obj) {
        var ret = admin.runCommand( Object.extend({ setParameter : 1 }, obj));
        return ret;
    }

    assert.commandWorked(setparam({"diagnosticDataCollectionEnabled": 1}));
    assert.commandWorked(setparam({"diagnosticDataCollectionPeriodMillis": 100}));
    assert.commandWorked(setparam({"diagnosticDataCollectionDirectorySizeMb": 10}));
    assert.commandWorked(setparam({"diagnosticDataCollectionFileSizeMb": 1}));
    assert.commandWorked(setparam({"diagnosticDataCollectionSamplesPerChunk": 1}));
    assert.commandWorked(setparam({"diagnosticDataCollectionSamplesPerInterimUpdate": 1}));

    // Negative tests - set values below minimums
    assert.commandFailed(setparam({"diagnosticDataCollectionPeriodMillis": 1}));
    assert.commandFailed(setparam({"diagnosticDataCollectionDirectorySizeMb": 1}));

    // Negative test - set file size bigger then directory size
    assert.commandWorked(setparam({"diagnosticDataCollectionDirectorySizeMb": 10}));
    assert.commandFailed(setparam({"diagnosticDataCollectionFileSizeMb": 100}));

    // Negative test - set directory size less then file size
    assert.commandWorked(setparam({"diagnosticDataCollectionDirectorySizeMb": 100}));
    assert.commandWorked(setparam({"diagnosticDataCollectionFileSizeMb": 50}));
    assert.commandFailed(setparam({"diagnosticDataCollectionDirectorySizeMb": 10}));

    // Reset
    assert.commandWorked(setparam({"diagnosticDataCollectionFileSizeMb": 10}));
    assert.commandWorked(setparam({"diagnosticDataCollectionDirectorySizeMb": 100}));
    assert.commandWorked(setparam({"diagnosticDataCollectionPeriodMillis": 1000}));
    assert.commandWorked(setparam({"diagnosticDataCollectionSamplesPerChunk": 300}));
    assert.commandWorked(setparam({"diagnosticDataCollectionSamplesPerInterimUpdate": 10}));
}) ();
