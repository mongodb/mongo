// Tests that query settings commands are not available in 'lastLTSFCV'. Ensures that previously set
// query settings are cleared upon downgrade and subsequent upgrade.
// @tags: [ requires_fcv_80 ]
//

import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

function assertQuerySettingsCount(qsutils, expectedQuerySettingsCount) {
    assert.soon(() => {
        return expectedQuerySettingsCount == qsutils.getQuerySettings().length;
    });
}

function runTest(conn) {
    const testDB = conn.getDB("test");
    const qsutils = new QuerySettingsUtils(testDB, "testColl");

    // Start in latest FCV,
    assert.commandWorked(
        testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    // Ensure users can specify query settings.
    assertQuerySettingsCount(qsutils, 0);
    assert.commandWorked(testDB.adminCommand(
        {setQuerySettings: {find: "test", $db: "db"}, settings: {queryFramework: "sbe"}}))
    assertQuerySettingsCount(qsutils, 1);

    // Downgrade the FCV.
    assert.commandWorked(
        testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    // Ensure users can not specify query settings.
    assert.commandFailedWithCode(
        testDB.adminCommand(
            {setQuerySettings: {find: "test", $db: "db"}, settings: {queryFramework: "sbe"}}),
        [7746400, ErrorCodes.BadValue]);

    // Upgrade the FCV. Ensure that old query settings are not present on upgrade.
    assert.commandWorked(
        testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    assertQuerySettingsCount(qsutils, 0);
}

(function runTestInReplicaSet() {
    const rst = new ReplSetTest({name: jsTestName(), nodes: [{binVersion: "latest"}]});
    rst.startSet();
    rst.initiate();

    runTest(rst.getPrimary());

    rst.stopSet();
})();

(function rutTestInShardedCluster() {
    const st = new ShardingTest({
        shards: 2,
        config: 1,
        other: {
            mongosOptions: {binVersion: "latest"},
            configOptions: {
                binVersion: "latest",
            },
            rsOptions: {
                binVersion: "latest",
            },
            rs: {nodes: 1},
        }
    });

    runTest(st.s);

    st.stop();
})();
