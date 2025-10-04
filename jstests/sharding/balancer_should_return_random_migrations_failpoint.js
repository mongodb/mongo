/**
 * Testing random migration failpoint
 * @tags: [
 *  requires_fcv_80,
 *  featureFlagTrackUnshardedCollectionsUponMoveCollection,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

// TODO SERVER-89399: re-enable the hook once it properly serialize with resharding operations
TestData.skipCheckShardFilteringMetadata = true;

// The mongod secondaries are set to priority 0 to prevent the primaries from stepping down during
// migrations on slow evergreen builders.
let st = new ShardingTest({
    shards: 2,
    other: {
        enableBalancer: true,
        configOptions: {
            setParameter: {
                "failpoint.balancerShouldReturnRandomMigrations": "{mode: 'alwaysOn'}",
                "reshardingMinimumOperationDurationMillis": 0,
                "balancerMigrationsThrottlingMs": 0,
            },
        },
        rsOptions: {setParameter: {"failpoint.balancerShouldReturnRandomMigrations": "{mode: 'alwaysOn'}"}},
    },
});

const dbNames = ["db0", "db1"];
const numDocuments = 25;
const timeFieldName = "time";

// TODO SERVER-84744 remove the feature flag
const isReshardingForTimeseriesEnabled = FeatureFlagUtil.isPresentAndEnabled(
    st.shard0.getDB("admin"),
    "ReshardingForTimeseries",
);

// Setup collections
{
    for (const dbName of dbNames) {
        let db = st.getDB(dbName);

        // Create unsharded collection
        let bulk = db.unsharded.initializeUnorderedBulkOp();
        for (let i = 0; i < numDocuments; ++i) {
            bulk.insert({"Surname": "Smith", "Age": i});
        }
        assert.commandWorked(bulk.execute());

        // Create sharded collection
        st.adminCommand({shardCollection: `${dbName}.sharded`, key: {x: 1}});

        // TODO (SERVER-84744): Remove check for feature flag
        if (isReshardingForTimeseriesEnabled) {
            // Create timeseries collection
            assert.commandWorked(db.createCollection("timeseries", {timeseries: {timeField: timeFieldName}}));
        }

        // Create view
        assert.commandWorked(db.createCollection("view", {viewOn: "unsharded"}));
    }
}

// Get the data shard for a given collection
function getPlacement(nss) {
    // wait until the collection gets tracked
    // We need to wait because collection get tracked only on the db
    assert.soon(() => {
        return st.s.getCollection("config.collections").countDocuments({_id: nss}) > 0;
    }, `Timed out waiting for collection ${nss} to get tracked`);

    let chunks = findChunksUtil.findChunksByNs(st.getDB("config"), nss).toArray();
    assert(chunks.length > 0, `Couldn't find chunk for collection ${nss}`);
    let shards = [];
    chunks.forEach((chunk) => {
        if (!shards.includes(chunk.shard)) {
            shards.push(chunk.shard);
        }
    });
    return {shards: shards, numChunks: chunks.length};
}

function placementDiffers(currentPlacement, initialPlacement) {
    let differs = false;
    if (currentPlacement.shards.length != initialPlacement.shards.length) {
        differs = true;
    }
    if (currentPlacement.numChunks != initialPlacement.numChunks) {
        differs = true;
    }
    initialPlacement.shards.forEach((shard) => {
        if (!currentPlacement.shards.includes(shard)) {
            differs = true;
        }
    });
    return differs;
}

// Store the initial shard for every namespace
// Map: namespace -> shardId
let initialPlacements = {};

let trackableCollections = ["unsharded", "sharded"];

if (isReshardingForTimeseriesEnabled) {
    let db = st.s.getDB(dbNames[0]);
    trackableCollections.push(getTimeseriesCollForDDLOps(db, db["timeseries"]).getName());
}

for (const dbName of dbNames) {
    for (const collName of trackableCollections) {
        const fullName = `${dbName}.${collName}`;
        initialPlacements[fullName] = getPlacement(fullName);
    }
}

jsTest.log(`Initial placement: ${tojson(initialPlacements)}`);

for (const [nss, initialPlacement] of Object.entries(initialPlacements)) {
    assert.soon(() => {
        let currentPlacement = getPlacement(nss);
        return placementDiffers(currentPlacement, initialPlacement);
    }, `Data shard for collection ${nss} didn't change`);
}

st.stop();
