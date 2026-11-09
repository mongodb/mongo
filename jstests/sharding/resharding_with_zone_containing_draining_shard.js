/**
 * Tests that resharding works when zones are defined and one of the shards in a zone is draining.
 *
 * A shard keeps its zone assignments while draining, but is not eligible to own any of the initial
 * chunks. The resulting data distribution must respect the zones and must not place any chunk on
 * the draining shard. If a zone has no shard left that can own chunks, resharding must fail with
 * an error rather than crashing the config server.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 3});

const dbName = "testDb";
const zoneName = "zoneA";
const configDB = st.s.getDB("config");

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

// removeShard puts shard2 in the draining state. It remains draining because ShardingTest disables
// the balancer by default and the test never issues a subsequent removeShard to advance removal.
const removeShardResult = assert.commandWorked(
    st.s.adminCommand({removeShard: st.shard2.shardName}),
);
assert.eq("started", removeShardResult.state, removeShardResult);

// A shard keeps its zone assignments while draining, so shard2 remains a member of zoneA.
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: zoneName}));

// Shards the given collection and assigns its whole key range to zoneA, so that every chunk
// created by resharding must be placed on one of that zone's shards.
function setUpCollection(collName) {
    const ns = dbName + "." + collName;
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

    assert.commandWorked(st.s.getDB(dbName)[collName].insert({oldKey: 1, newKey: 1}));

    assert.commandWorked(
        st.s.adminCommand({
            updateZoneKeyRange: ns,
            min: {oldKey: MinKey},
            max: {oldKey: MaxKey},
            zone: zoneName,
        }),
    );
    return ns;
}

const reshardZones = [{zone: zoneName, min: {newKey: MinKey}, max: {newKey: MaxKey}}];

// Returns the shards owning chunks of 'ns'.
function getChunkShards(ns) {
    const collEntry = configDB.collections.findOne({_id: ns});
    assert(collEntry, "no config.collections entry", {ns});
    return configDB.chunks.distinct("shard", {uuid: collEntry.uuid});
}

function assertConfigServerIsHealthy() {
    assert.commandWorked(st.configRS.getPrimary().adminCommand({ping: 1}));
    assert.eq(
        0,
        st.configRS.getPrimary().getDB("config").reshardingOperations.countDocuments({}),
        "a resharding coordinator document was left behind",
    );
}

jsTest.log("Resharding when the zone contains both a draining and a non-draining shard");
{
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: zoneName}));

    const ns = setUpCollection("collWithEligibleShard");
    assert.commandWorked(
        st.s.adminCommand({
            reshardCollection: ns,
            key: {newKey: 1},
            zones: reshardZones,
            numInitialChunks: 1,
        }),
    );

    // Every chunk must land on shard1: shard2 is in the zone but is draining, and shard0 is not in
    // the zone at all.
    assert.eq([st.shard1.shardName], getChunkShards(ns));

    assertConfigServerIsHealthy();

    assert.commandWorked(
        st.s.adminCommand({removeShardFromZone: st.shard1.shardName, zone: zoneName}),
    );
}

jsTest.log("Resharding when the only shard in the zone is draining");
{
    const ns = setUpCollection("collWithNoEligibleShard");

    // No chunk can be placed, so resharding must fail rather than crash.
    assert.commandFailedWithCode(
        st.s.adminCommand({
            reshardCollection: ns,
            key: {newKey: 1},
            zones: reshardZones,
            numInitialChunks: 1,
        }),
        ErrorCodes.InvalidOptions,
    );

    assertConfigServerIsHealthy();
}

st.stop();
