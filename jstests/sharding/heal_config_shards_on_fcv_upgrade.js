/*
 * Tests the fix-up mechanism added to the upgrade path on setFCV(9.0), which adds a topologyTime
 * field to config.shards entries which do not have one due to originating from a pre-5.0 cluster.
 *
 * TODO (SERVER-102087): remove after 9.0 is branched.
 *
 *  @tags: [
 *      multiversion_incompatible,
 *  ]
 * */
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runConfigShardsFixUpTest() {
    const st = new ShardingTest({
        mongos: 1,
        shards: 2,
        config: 3,
        rs: {nodes: 1},
    });

    function triggerSelfHeal() {
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: "8.0", confirm: true}));
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    }

    let configPrimary = st.configRS.getPrimary();
    let latestTopologyTime;

    jsTest.log("Removing topologyTime from all entries in config.shards");
    {
        // Remove topologyTime to simulate config.shards with no topologyTime due to an upgrade from
        // an old version (<5.0).
        configPrimary.getDB("config")["shards"].updateMany({}, {$unset: {topologyTime: ""}});

        // Double check 'config.shards' has no entries with a topologyTime.
        let shardDocs = configPrimary.getDB("config")["shards"].find().toArray();
        shardDocs.forEach(doc => assert.eq(doc.hasOwnProperty("topologyTime"), false, shardDocs));

        triggerSelfHeal();

        // Check every document has a topologyTime.
        shardDocs = configPrimary.getDB("config")["shards"].find().toArray();
        shardDocs.forEach(doc => assert.eq(doc.hasOwnProperty("topologyTime"), true, shardDocs));

        latestTopologyTime = shardDocs[0].topologyTime;
        // Make sure the written topologyTime is gossiped.
        assert.soon(() => {
            let ss = st.s.adminCommand({serverStatus: 1});
            return timestampCmp(ss.sharding.lastSeenTopologyOpTime.ts, latestTopologyTime) == 0;
        });
    }

    jsTest.log("Removing topologyTime from a single entry in config.shards");
    {
        // Test that if some of the entries already have a topologyTime, it is not overwritten.
        // Update one, leaving the other untouched. From the previous step, we know all topology
        // times are the same, so we won't cause any monotonicity issues.
        jsTest.log(
            configPrimary.getDB("config")["shards"].updateOne({}, {$unset: {topologyTime: ""}}));
        const docWithTopologyTimeBefore = configPrimary.getDB("config")["shards"]
                                              .find({topologyTime: latestTopologyTime})
                                              .toArray()[0];

        triggerSelfHeal();

        // Check every document has a topologyTime.
        let shardDocs = configPrimary.getDB("config")["shards"].find().toArray();
        shardDocs.forEach(doc => assert.eq(doc.hasOwnProperty("topologyTime"), true, shardDocs));

        // The doc which had a topologyTime is untouched.
        assert.eq(shardDocs.find(doc => timestampCmp(doc.topologyTime, latestTopologyTime) == 0),
                  docWithTopologyTimeBefore);

        // The other doc has a greater topologyTime.
        const updatedDoc =
            shardDocs.find(doc => timestampCmp(doc.topologyTime, latestTopologyTime) != 0);
        assert.gt(updatedDoc.topologyTime, latestTopologyTime);
    }

    st.stop();
}

runConfigShardsFixUpTest();
