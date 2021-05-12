// Verifies the serverStatus output and FTDC output for the read write concern defaults.
//
// TODO SERVER-45052: Split this test into the appropriate suites for replica sets, sharded
// clusters, and standalone servers.
//
// @tags: [requires_sharding]
(function() {
"use strict";

load("jstests/libs/ftdc.js");
load("jstests/libs/write_concern_util.js");  // For isDefaultWriteConcernMajorityFlagEnabled.
load('jstests/replsets/rslib.js');           // For isDefaultReadConcernLocalFlagEnabled.

// Verifies the transaction server status response has the fields that we expect.
function verifyServerStatus(conn,
                            {expectedRC, expectedWC, expectNoDefaultsDocument, expectNothing},
                            isImplicitDefaultWCMajority) {
    const res = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    if (expectNothing) {
        assert.eq(undefined, res.defaultRWConcern, tojson(res.defaultRWConcern));
        return;
    }

    assert.hasFields(res, ["defaultRWConcern"]);
    const defaultsRes = res.defaultRWConcern;

    if (isDefaultReadConcernLocalFlagEnabled(conn)) {
        assert.hasFields(defaultsRes, ["defaultReadConcernSource"]);
        if (!expectedRC) {
            assert.eq("implicit", defaultsRes.defaultReadConcernSource, tojson(defaultsRes));
            expectedRC = {level: "local"};
        } else {
            assert.eq("global", defaultsRes.defaultReadConcernSource, tojson(defaultsRes));
        }
    } else {
        assert.eq(undefined, defaultsRes.defaultReadConcernSource);
    }

    if (expectedRC) {
        assert.eq(expectedRC, defaultsRes.defaultReadConcern, tojson(defaultsRes));
    } else {
        assert.eq(undefined, defaultsRes.defaultReadConcern, tojson(defaultsRes));
    }

    if (isDefaultWriteConcernMajorityFlagEnabled(conn)) {
        assert.hasFields(defaultsRes, ["defaultWriteConcernSource"]);
        if (!expectedWC) {
            assert.eq("implicit", defaultsRes.defaultWriteConcernSource, tojson(defaultsRes));
            if (isImplicitDefaultWCMajority) {
                expectedWC = {w: "majority", wtimeout: 0};
            }
        } else {
            assert.eq("global", defaultsRes.defaultWriteConcernSource, tojson(defaultsRes));
        }
    } else {
        assert.eq(undefined, defaultsRes.defaultWriteConcernSource);
    }

    if (expectedWC) {
        assert.eq(expectedWC, defaultsRes.defaultWriteConcern, tojson(defaultsRes));
    } else {
        assert.eq(undefined, defaultsRes.defaultWriteConcern, tojson(defaultsRes));
    }

    if (expectNoDefaultsDocument) {
        // When defaults have never been set (or the defaults document was deleted) the response
        // should only contain localUpdateWallClockTime.
        assert.hasFields(defaultsRes, ["localUpdateWallClockTime"]);
        assert.eq(undefined, defaultsRes.updateWallClockTime, tojson(defaultsRes));
        assert.eq(undefined, defaultsRes.updateOpTime, tojson(defaultsRes));
    } else {
        // These fields are always set once a read or write concern has been set at least once and
        // the defaults document is still present.
        assert.hasFields(defaultsRes,
                         ["updateOpTime", "updateWallClockTime", "localUpdateWallClockTime"]);
    }
}

function testServerStatus(conn, isImplicitDefaultWCMajority) {
    // When no defaults have been set.
    verifyServerStatus(conn, {expectNoDefaultsDocument: true}, isImplicitDefaultWCMajority);

    // When only read concern is set.
    assert.commandWorked(
        conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "majority"}}));
    verifyServerStatus(conn, {expectedRC: {level: "majority"}}, isImplicitDefaultWCMajority);

    // When read concern is explicitly unset and write concern is unset.
    assert.commandWorked(conn.adminCommand(
        {setDefaultRWConcern: 1, defaultReadConcern: {}, defaultWriteConcern: {}}));
    verifyServerStatus(conn, {}, isImplicitDefaultWCMajority);

    // When only write concern is set.
    assert.commandWorked(conn.adminCommand(
        {setDefaultRWConcern: 1, defaultReadConcern: {}, defaultWriteConcern: {w: "majority"}}));
    verifyServerStatus(
        conn, {expectedWC: {w: "majority", wtimeout: 0}}, isImplicitDefaultWCMajority);

    // When both read and write concern are set.
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "majority"},
        defaultWriteConcern: {w: "majority"}
    }));
    verifyServerStatus(conn,
                       {expectedRC: {level: "majority"}, expectedWC: {w: "majority", wtimeout: 0}},
                       isImplicitDefaultWCMajority);

    // When the defaults document has been deleted.
    assert.commandWorked(conn.getDB("config").settings.remove({_id: "ReadWriteConcernDefaults"}));
    assert.soon(() => {
        // Wait for the cache to discover the defaults were deleted. Note the cache is invalidated
        // on delete on a mongod, so this is only to handle the mongos case.
        const res = conn.adminCommand({getDefaultRWConcern: 1, inMemory: true});
        return !res.hasOwnProperty("updateOpTime");
    }, "mongos failed to pick up deleted default rwc", undefined, 1000, {runHangAnalyzer: false});
    verifyServerStatus(conn, {expectNoDefaultsDocument: true}, isImplicitDefaultWCMajority);
}

function testFTDC(conn, ftdcDirPath, expectNothingOnRotation = false) {
    //
    // The periodic samples used for FTDC shouldn't include the default read write concerns.
    //

    const latestPeriodicFTDC = verifyGetDiagnosticData(conn.getDB("admin"));
    assert.hasFields(latestPeriodicFTDC, ["serverStatus"]);
    assert.eq(undefined,
              latestPeriodicFTDC.serverStatus.defaultRWConcern,
              tojson(latestPeriodicFTDC.serverStatus));

    //
    // The first sample in the FTDC file should have the default read write concern, if the node is
    // expected to collect it on log file rotation.
    //

    const ftdcFiles = listFiles(ftdcDirPath);

    // Read from the first non-interim file.
    const firstFullFile =
        ftdcFiles.filter(fileDesc => fileDesc.baseName.indexOf("interim") == -1)[0];
    const ftdcData = _readDumpFile(firstFullFile.name);
    assert.hasFields(ftdcData[0], ["doc"], tojson(ftdcData));

    // Look for the defaults in the first metadata object.
    const firstMetadataObj = ftdcData.filter(obj => obj.type == 0)[0];
    assert.hasFields(firstMetadataObj, ["doc"]);

    if (expectNothingOnRotation) {
        assert.eq(undefined, firstMetadataObj.doc.getDefaultRWConcern, tojson(ftdcData[0].doc));
    } else {
        assert.hasFields(firstMetadataObj.doc, ["getDefaultRWConcern"]);
    }
}

jsTestLog("Testing sharded cluster...");
{
    const testPathMongos = MongoRunner.toRealPath("ftdc_dir_mongos");
    const testPathConfig = MongoRunner.toRealPath("ftdc_dir_config");
    const testPathShard = MongoRunner.toRealPath("ftdc_dir_shard");
    const st = new ShardingTest({
        shards: [{setParameter: {diagnosticDataCollectionDirectoryPath: testPathShard}}],
        mongos: {
            s0: {setParameter: {diagnosticDataCollectionDirectoryPath: testPathMongos}},
        },
        config: 1,
        configOptions: {setParameter: {diagnosticDataCollectionDirectoryPath: testPathConfig}}
    });

    testServerStatus(st.s, true /* isImplicitDefaultWCMajority */);
    testFTDC(st.s, testPathMongos);

    testServerStatus(st.configRS.getPrimary(), true /* isImplicitDefaultWCMajority */);
    testFTDC(st.configRS.getPrimary(), testPathConfig);

    // Set a default before verifying it isn't included by shards.
    assert.commandWorked(
        st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority"}}));
    verifyServerStatus(
        st.rs0.getPrimary(), {expectNothing: true}, true /* isImplicitDefaultWCMajority */);
    testFTDC(st.rs0.getPrimary(), testPathShard, true /* expectNothingOnRotation */);

    st.stop();
}

jsTestLog("Testing plain replica set...");
{
    const testPath = MongoRunner.toRealPath("ftdc_dir_repl_node");
    const rst = new ReplSetTest(
        {nodes: 1, nodeOptions: {setParameter: {diagnosticDataCollectionDirectoryPath: testPath}}});
    rst.startSet();
    rst.initiate();

    testServerStatus(rst.getPrimary(), true /* isImplicitDefaultWCMajority */);
    testFTDC(rst.getPrimary(), testPath);

    rst.stopSet();
}

jsTestLog("Testing server status for plain replica set with implicit default WC {w:1}");
{
    const rst = new ReplSetTest({nodes: [{}, {}, {arbiter: true}]});
    rst.startSet();
    rst.initiate();

    testServerStatus(rst.getPrimary(), false /* isImplicitDefaultWCMajority */);
    rst.stopSet();
}

jsTestLog("Testing standalone server...");
{
    const testPath = MongoRunner.toRealPath("ftdc_dir_standalone");
    const standalone =
        MongoRunner.runMongod({setParameter: {diagnosticDataCollectionDirectoryPath: testPath}});

    verifyServerStatus(standalone, {expectNothing: true}, true /* isImplicitDefaultWCMajority */);
    testFTDC(standalone, testPath, true /* expectNothingOnRotation */);

    MongoRunner.stopMongod(standalone);
}
}());
