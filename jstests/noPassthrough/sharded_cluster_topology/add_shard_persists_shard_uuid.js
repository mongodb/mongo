/**
 * Tests that when featureFlagUniqueShardIdentifiers* is enabled, the addShard and
 * transitionFromDedicatedConfigServer operations persist a unique uuid in config.shards and
 * propagate it into the shard's identity document on the newly added shard. Also verifies that the
 * shardingState command reports the same shard UUID.
 *
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagUniqueShardIdentifiers,
 *   featureFlagUniqueShardIdentifiersDDL,
 *   config_shard_incompatible,
 *   # This test calls addShard manually and checks sharding metadata.
 *   assumes_stable_shard_list,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/** Return whether the value is a UUID string (36-char canonical form). */
function isUUIDString(val) {
    return (
        typeof val === "string" &&
        /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(val)
    );
}

function getUuidValueFromBsonField(bsonField) {
    assert(
        bsonField instanceof BinData && bsonField.subtype() === 4,
        "expected uuid to be a UUID BinData",
        {
            bsonField,
        },
    );

    // BinData.toString() returns "UUID(\"<canonical string>\")" in the shell.
    const uuidStr = bsonField.toString().replace(/^UUID\("(.+)"\)$/, "$1");
    assert(isUUIDString(uuidStr), "expected a canonical UUID string", {uuidStr});
    return uuidStr;
}

/**
 * Assert that the actual UUID is a BSON UUID (BinData subtype 4) and matches the expected UUID string.
 */
function assertUUIDMatches(expectedUuidStr, actualUuid, message) {
    assert.eq(expectedUuidStr, getUuidValueFromBsonField(actualUuid), message);
}

/**
 * Assert that the uuid stored in config.shards for `shardName` is a BSON UUID (BinData subtype
 * 4) and return its canonical string representation so callers can cross-check other collections.
 */
function assertAndGetShardUuid(configDb, shardName) {
    const shardDoc = configDb.shards.findOne({_id: shardName});
    assert(shardDoc, "expected config.shards entry for shard", {shardName});
    assert(shardDoc.uuid !== undefined, "expected uuid field to be set in config.shards", {
        shardName,
        shardDoc,
    });

    const uuid = shardDoc.uuid;
    return getUuidValueFromBsonField(uuid);
}

// ---------------------------------------------------------------------------
// Test: normal addShard – a UUID is persisted in both config.shards and the
//       shardIdentity document on the new shard.
// ---------------------------------------------------------------------------
jsTest.log.info("addShard persists a UUID in config.shards and the shardIdentity document");
{
    const st = new ShardingTest({
        shards: 0,
        mongos: 1,
    });
    const configDb = st.s.getDB("config");

    const rs = new ReplSetTest({nodes: 1});
    rs.startSet({shardsvr: ""});
    rs.initiate();

    const shardName = "myShardWithUUID";
    assert.commandWorked(st.s.adminCommand({addShard: rs.getURL(), name: shardName}));

    // Verify config.shards carries a UUID.
    const configUuidStr = assertAndGetShardUuid(configDb, shardName);
    jsTest.log.info("config.shards uuid", {configUuidStr});

    // Verify the shard identity document on the added shard also carries the uuid and that
    // both values agree.
    const identityDoc = rs
        .getPrimary()
        .getDB("admin")
        .system.version.findOne({_id: "shardIdentity"});
    assert(identityDoc, "expected shardIdentity document on the added shard");
    assertUUIDMatches(
        configUuidStr,
        identityDoc.uuid,
        "uuid in config.shards and shardIdentity on the shard must match",
    );

    const shardingState = assert.commandWorked(rs.getPrimary().adminCommand({shardingState: 1}));
    assert(shardingState.enabled, "shardingState should report enabled on the added shard");
    assert.eq(shardName, shardingState.shardName);
    assert(shardingState.shardUuid !== undefined, "expected shardUuid in shardingState response", {
        shardingState,
    });
    assertUUIDMatches(
        configUuidStr,
        shardingState.shardUuid,
        "shardUuid from shardingState must match config.shards",
    );

    rs.stopSet();
    st.stop();
}

// ---------------------------------------------------------------------------
// Test: promotion to sharded – adding a replica set that already contains user
//       data as the first shard (a "promotion") also persists a UUID in both
//       config.shards and the shardIdentity document on the promoted shard.
// ---------------------------------------------------------------------------
jsTest.log.info("Promoting a replica set with data to a sharded cluster persists a UUID");
{
    const st = new ShardingTest({
        shards: 0,
        mongos: 1,
    });
    const configDb = st.s.getDB("config");

    const rs = new ReplSetTest({nodes: 1});
    rs.startSet({shardsvr: ""});
    rs.initiate();

    // Write user data to the replica set prior to adding.
    const userDbName = jsTestName() + "_promotion";
    assert.commandWorked(
        rs.getPrimary().getDB(userDbName).coll.insert({_id: 1, value: "pre-existing"}),
    );
    rs.awaitReplication();

    const shardName = "promotedShardWithUUID";
    assert.commandWorked(st.s.adminCommand({addShard: rs.getURL(), name: shardName}));

    // Verify config.shards carries a UUID.
    const configUuidStr = assertAndGetShardUuid(configDb, shardName);
    jsTest.log.info("config.shards uuid after promotion", {configUuidStr});

    // Verify the shard identity document on the promoted shard also carries the uuid and that both
    // values agree.
    const identityDoc = rs
        .getPrimary()
        .getDB("admin")
        .system.version.findOne({_id: "shardIdentity"});
    assert(identityDoc, "expected shardIdentity document on the promoted shard");
    assertUUIDMatches(
        configUuidStr,
        identityDoc.uuid,
        "uuid in config.shards and shardIdentity on the promoted shard must match",
    );

    // Sanity check that the pre-existing user data survived the promotion by reading it back
    // directly from the (now promoted) shard. This confirms the addShard was treated as a
    // promotion of an existing replica set rather than the addition of an empty shard.
    // TODO SERVER-127411: Replace this with a sanity check that the data is routable through mongos.
    assert.eq(
        {_id: 1, value: "pre-existing"},
        rs.getPrimary().getDB(userDbName).coll.findOne({_id: 1}),
        "expected pre-existing user data to survive the promotion",
    );

    // The config.databases entry for the pre-existing database must reference its primary shard by
    // the shard's UUID (BinData subtype 4) rather than by the legacy shard name string, and that
    // UUID must match the one persisted in config.shards.
    const dbDoc = configDb.databases.findOne({_id: userDbName});
    assert(dbDoc, "expected config.databases entry for the pre-existing database", {userDbName});
    assertUUIDMatches(
        configUuidStr,
        dbDoc.primary,
        "config.databases primary must match the promoted shard's uuid in config.shards",
    );

    // The config.placementHistory entry for the pre-existing database must record its placement
    // using the shard's UUID rather than the legacy shard name string. The latest entry for the
    // database namespace should list exactly the promoted shard.
    const placementEntry = configDb.placementHistory
        .find({nss: userDbName})
        .sort({timestamp: -1})
        .limit(1)
        .toArray()[0];
    assert(placementEntry, "expected config.placementHistory entry for the pre-existing database", {
        userDbName,
    });
    assert.eq(1, placementEntry.shards.length, "expected a single shard in the placement entry", {
        placementEntry,
    });
    assertUUIDMatches(
        configUuidStr,
        placementEntry.shards[0],
        "config.placementHistory shard must match the promoted shard's uuid in config.shards",
    );

    rs.stopSet();
    st.stop();
}

// ---------------------------------------------------------------------------
// Test: transition from dedicated – a UUID is generated when the config
//       server is initialized and it is persisted in config.shards during the
//       transition from dedicated.
// ---------------------------------------------------------------------------
jsTest.log.info("Transition from dedicated persists a UUID in config.shards");
{
    const st = new ShardingTest({
        shards: 0,
        mongos: 1,
    });
    const configDb = st.s.getDB("config");

    // Verify that the shard identity already contains a UUID.
    const identityDoc = st.configRS
        .getPrimary()
        .getDB("admin")
        .system.version.findOne({_id: "shardIdentity"});
    assert(identityDoc, "expected shardIdentity document on the config server");
    assert(identityDoc.uuid !== undefined, "expected uuid field to be set in shardIdentity", {
        identityDoc,
    });

    assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));

    // Verify config.shards carries a UUID and it matches the one in the shard identity.
    const configUuidStr = assertAndGetShardUuid(configDb, "config");
    assertUUIDMatches(
        configUuidStr,
        identityDoc.uuid,
        "uuid in shardIdentity and config.shards must match",
    );

    // Verify the shardingState command reports the same shard UUID.
    const shardingState = assert.commandWorked(
        st.configRS.getPrimary().adminCommand({shardingState: 1}),
    );
    assert(shardingState.enabled, "shardingState should report enabled on the config server");
    assert.eq("config", shardingState.shardName);
    assert(shardingState.shardUuid !== undefined, "expected shardUuid in shardingState response", {
        shardingState,
    });
    assertUUIDMatches(
        configUuidStr,
        shardingState.shardUuid,
        "shardUuid from shardingState must match config.shards on the config server",
    );

    st.stop();
}
