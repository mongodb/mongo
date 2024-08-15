/**
 * Test that setClusterParameter will fail on multiversion clusters where the cluster FCV does not
 * meet the minimum FCV needed on a cluster parameter.
 */

// Test sharding cluster with multiple versions should fail since the FCV will be last-lts and the
// command needs the latest.
{
    jsTestLog('Running multiversion cluster test');
    const st = new ShardingTest({
        shards: {
            rs0: {nodes: [{binVersion: "latest"}, {binVersion: "last-lts"}]},
            rs1: {nodes: [{binVersion: "latest"}]}
        },
        other: {mongosOptions: {binVersion: "last-lts"}}
    });

    const adminDB = st.s.getDB("admin");

    // Assert cluster is running the last-lts FCV.
    const version =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1}))
            .featureCompatibilityVersion.version;

    assert.eq(version, lastLTSFCV, "Cluster is not running the last-lts FCV");

    assert.commandFailedWithCode(
        adminDB.runCommand({setClusterParameter: {cwspTestNeedsLatestFCV: {intData: 106}}}),
        ErrorCodes.BadValue);

    st.stop();
}

// Test sharding cluster with latest FCV should succeed setting the parameter.
{
    jsTestLog('Running latest FCV cluster test');
    const st = new ShardingTest({
        shards: {
            rs0: {nodes: [{binVersion: "latest"}, {binVersion: "latest"}]},
            rs1: {nodes: [{binVersion: "latest"}]}
        },
        other: {mongosOptions: {binVersion: "latest"}}
    });
    const adminDB = st.s.getDB("admin");

    // Assert cluster is running the latest FCV.
    const version =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1}))
            .featureCompatibilityVersion.version;

    assert.eq(version, latestFCV, "Cluster is not running the latest FCV");

    assert.commandWorked(
        adminDB.runCommand({setClusterParameter: {cwspTestNeedsLatestFCV: {intData: 106}}}));

    st.stop();
}
