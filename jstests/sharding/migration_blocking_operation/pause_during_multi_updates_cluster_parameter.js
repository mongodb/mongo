/*
 * Tests that pauseMigrationsDuringMultiUpdates cluster parameter is off by default.
 * @tags: [
 *  featureFlagPauseMigrationsDuringMultiUpdatesAvailable,
 *  requires_fcv_80
 * ]
 */

const numShards = 1;
const st = new ShardingTest({shards: numShards});
const allRs = [st.configRS];
for (let i = 0; i < numShards; i++) {
    allRs.push(st[`rs${i}`]);
}

// Ensure pauseMigrationsDuringMultiUpdates defaults to false.
for (const rs of allRs) {
    const response = assert.commandWorked(
        rs.getPrimary().adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}));
    assert.eq(response.clusterParameters[0].enabled, false);
}

st.stop();
