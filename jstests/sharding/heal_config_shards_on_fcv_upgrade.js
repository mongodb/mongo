/*
 * Tests the fix-up mechanism added to the upgrade path on setFCV(9.0), which adds a topologyTime
 * field to config.shards entries which do not have one due to originating from a pre-5.0 cluster.
 *
 * TODO (SERVER-102087): remove multiversion_incompatible after 9.0 is branched.
 * TODO (SERVER-120391): remove does_not_support_stepdowns.
 *  @tags: [
 *      multiversion_incompatible,
 *      does_not_support_stepdowns,
 *  ]
 * */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

function runConfigShardsFixUpTest() {
    const st = new ShardingTest({
        mongos: 1,
        shards: 2,
        config: 3,
        rs: {nodes: 1},
    });

    function triggerSelfHeal() {
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: "8.0", confirm: true}),
        );
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
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
        shardDocs.forEach((doc) => assert.eq(doc.hasOwnProperty("topologyTime"), false, shardDocs));

        triggerSelfHeal();

        // Check every document has a topologyTime.
        shardDocs = configPrimary.getDB("config")["shards"].find().toArray();
        shardDocs.forEach((doc) => assert.eq(doc.hasOwnProperty("topologyTime"), true, shardDocs));

        // Take the maximum persisted topology time (which is the one backing the vector clock / ShardRegistry timeInStore).
        latestTopologyTime = FixtureHelpers.getTopologyTime(configPrimary.getDB("config"));
        // Make sure the written topologyTime is gossiped.
        assert.soon(() => {
            let ss = st.s.adminCommand({serverStatus: 1});
            return timestampCmp(ss.sharding.lastSeenTopologyOpTime.ts, latestTopologyTime) == 0;
        });
    }

    jsTest.log("Removing topologyTime from a single entry in config.shards");
    {
        // Test the healing when only some of the entries are missing a topologyTime.
        // Unset the topologyTime of an entry, ensuring that there is at least one document holding the cluster's maximum topologyTime.
        // This keeps the collection's max topologyTime unchanged, so we don't lower it below the ShardRegistry's
        // timeInStore and avoid a monotonicity violation on the next reload.
        const docToUnset = configPrimary
            .getDB("config")
            ["shards"].find()
            .sort({topologyTime: -1})
            .toArray()[1];
        jsTest.log(
            configPrimary
                .getDB("config")
                ["shards"].updateOne({_id: docToUnset._id}, {$unset: {topologyTime: ""}}),
        );

        triggerSelfHeal();

        // Check every document has a topologyTime.
        let shardDocs = configPrimary.getDB("config")["shards"].find().toArray();
        shardDocs.forEach((doc) => assert.eq(doc.hasOwnProperty("topologyTime"), true, shardDocs));

        // Healing is monotonic: no entry regressed below the previous maximum topologyTime.
        shardDocs.forEach((doc) =>
            assert.gte(timestampCmp(doc.topologyTime, latestTopologyTime), 0, shardDocs),
        );
    }

    st.stop();
}

runConfigShardsFixUpTest();
