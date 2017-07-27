/**
 * Test that verifies FTDC works in mongos.
 */
load('jstests/libs/ftdc.js');

(function() {
    'use strict';
    let testPath1 = MongoRunner.toRealPath('ftdc_setdir1');
    let testPath2 = MongoRunner.toRealPath('ftdc_setdir2');
    let testPath3 = MongoRunner.toRealPath('ftdc_setdir3');
    let testLog3 = testPath3 + "mongos_ftdc.log";

    // Make the log file directory for mongos.
    mkdir(testPath3);

    // Startup 3 mongos:
    // 1. Normal MongoS with no log file to verify FTDC can be startup at runtime with a path.
    // 2. MongoS with explict diagnosticDataCollectionDirectoryPath setParameter at startup.
    // 3. MongoS with log file to verify automatic FTDC path computation works.
    let st = new ShardingTest({
        shards: 1,
        mongos: {
            s0: {verbose: 0},
            s1: {setParameter: {diagnosticDataCollectionDirectoryPath: testPath2}},
            s2: {logpath: testLog3}
        }
    });

    let admin1 = st.s0.getDB('admin');
    let admin2 = st.s1.getDB('admin');
    let admin3 = st.s2.getDB('admin');

    function setParam(admin, obj) {
        var ret = admin.runCommand(Object.extend({setParameter: 1}, obj));
        return ret;
    }

    function getParam(admin, field) {
        var q = {getParameter: 1};
        q[field] = 1;

        var ret = admin.runCommand(q);
        assert.commandWorked(ret);
        return ret[field];
    }

    // Verify FTDC can be started at runtime.
    function verifyFTDCDisabledOnStartup() {
        jsTestLog("Running verifyFTDCDisabledOnStartup");
        verifyCommonFTDCParameters(admin1, false);

        // 1. Try to enable and fail
        assert.commandFailed(setParam(admin1, {"diagnosticDataCollectionEnabled": 1}));

        // 2. Set path and succeed
        assert.commandWorked(
            setParam(admin1, {"diagnosticDataCollectionDirectoryPath": testPath1}));

        // 3. Set path again and fail
        assert.commandFailed(
            setParam(admin1, {"diagnosticDataCollectionDirectoryPath": testPath1}));

        // 4. Enable successfully
        assert.commandWorked(setParam(admin1, {"diagnosticDataCollectionEnabled": 1}));

        // 5. Validate getDiagnosticData returns FTDC data now
        jsTestLog("Verifying FTDC getDiagnosticData");
        verifyGetDiagnosticData(admin1);
    }

    // Verify FTDC is already running if there was a path set at startup.
    function verifyFTDCStartsWithPath() {
        jsTestLog("Running verifyFTDCStartsWithPath");
        verifyCommonFTDCParameters(admin2, true);

        // 1. Set path fail
        assert.commandFailed(
            setParam(admin2, {"diagnosticDataCollectionDirectoryPath": testPath2}));

        // 2. Enable successfully
        assert.commandWorked(setParam(admin2, {"diagnosticDataCollectionEnabled": 1}));

        // 3. Validate getDiagnosticData returns FTDC data now
        jsTestLog("Verifying FTDC getDiagnosticData");
        verifyGetDiagnosticData(admin2);
    }

    function normpath(path) {
        return path.replace(/\\/g, "/");
    }

    // Verify FTDC is already running if there was a path set at startup.
    function verifyFTDCStartsWithLogFile() {
        jsTestLog("Running verifyFTDCStartsWithLogFile");
        verifyCommonFTDCParameters(admin3, true);

        // 1. Verify that path is computed correctly.
        let computedPath = getParam(admin3, "diagnosticDataCollectionDirectoryPath");
        assert.eq(normpath(computedPath), normpath(testPath3 + "mongos_ftdc.diagnostic.data"));

        // 2. Set path fail
        assert.commandFailed(
            setParam(admin3, {"diagnosticDataCollectionDirectoryPath": testPath3}));

        // 3. Enable successfully
        assert.commandWorked(setParam(admin3, {"diagnosticDataCollectionEnabled": 1}));

        // 4. Validate getDiagnosticData returns FTDC data now
        jsTestLog("Verifying FTDC getDiagnosticData");
        verifyGetDiagnosticData(admin3);
    }

    verifyFTDCDisabledOnStartup();
    verifyFTDCStartsWithPath();
    verifyFTDCStartsWithLogFile();

    st.stop();
})();
