// Continuation of max_time_ms.js, but only contains commands which are not present in a previous
// version
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2});

    var mongos = st.s0;
    var shards = [st.shard0, st.shard1];
    var coll = mongos.getCollection("foo.bar");
    var admin = mongos.getDB("admin");
    var cursor;
    var res;

    // Helper function to configure "maxTimeAlwaysTimeOut" fail point on shards, which forces mongod
    // to throw if it receives an operation with a max time.  See fail point declaration for
    // complete description.
    var configureMaxTimeAlwaysTimeOut = function(mode) {
        assert.commandWorked(shards[0].getDB("admin").runCommand(
            {configureFailPoint: "maxTimeAlwaysTimeOut", mode: mode}));
        assert.commandWorked(shards[1].getDB("admin").runCommand(
            {configureFailPoint: "maxTimeAlwaysTimeOut", mode: mode}));
    };

    // Positive test for "setFeatureCompatibilityVersion"
    configureMaxTimeAlwaysTimeOut("alwaysOn");
    assert.commandFailedWithCode(
        admin.runCommand({setFeatureCompatibilityVersion: '3.2', maxTimeMS: 1000 * 60 * 60 * 24}),
        ErrorCodes.ExceededTimeLimit,
        "expected setFeatureCompatibilityVersion to fail due to maxTimeAlwaysTimeOut fail point");

    // Negative test for "setFeatureCompatibilityVersion"
    configureMaxTimeAlwaysTimeOut("off");
    assert.commandWorked(
        admin.runCommand({setFeatureCompatibilityVersion: '3.2', maxTimeMS: 1000 * 60 * 60 * 24}),
        "expected setFeatureCompatibilityVersion to not hit time limit in mongod");

    st.stop();

})();
