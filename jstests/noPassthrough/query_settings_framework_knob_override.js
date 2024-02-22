// Tests that query settings 'queryFramework' overrides framework control knob.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   simulate_atlas_proxy_incompatible,
//   cqf_incompatible,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

function test(db) {
    // Create the collection, because some sharding passthrough suites are failing when explain
    // command is issued on the nonexistent database and collection.
    const coll = assertDropAndRecreateCollection(db, jsTestName());
    const qsutils = new QuerySettingsUtils(db, coll.getName());

    const sbeEligibleQuery = {
        find: coll.getName(),
        $db: db.getName(),
        filter: {a: {$lt: 2}},
    };

    const sbeRestrictedQuery = qsutils.makeAggregateQueryInstance(
        {pipeline: [{$group: {_id: "$groupID", count: {$sum: 1}}}]});

    qsutils.testQueryFramework({
        query: sbeEligibleQuery,
        settings: {queryFramework: "classic"},
        knob: "trySbeRestricted",
        expectedEngine: "classic",
    });

    qsutils.testQueryFramework({
        query: sbeEligibleQuery,
        settings: {queryFramework: "sbe"},
        knob: "trySbeRestricted",
        expectedEngine: "sbe",
    });

    qsutils.testQueryFramework({
        query: sbeEligibleQuery,
        settings: {queryFramework: "sbe"},
        knob: "forceClassicEngine",
        expectedEngine: "sbe",
    });

    qsutils.testQueryFramework({
        query: sbeRestrictedQuery,
        settings: {queryFramework: "classic"},
        knob: "trySbeRestricted",
        expectedEngine: "classic",
    });

    qsutils.testQueryFramework({
        query: sbeRestrictedQuery,
        knob: "trySbeRestricted",
        expectedEngine: "sbe",
    });
}

{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    test(rst.getPrimary().getDB("ReplSetTestDB"));
    rst.stopSet();
}

{
    const st = new ShardingTest({shards: 3, mongos: 1});
    test(st.getDB("ShardingTestDB"));
    st.stop()
}
