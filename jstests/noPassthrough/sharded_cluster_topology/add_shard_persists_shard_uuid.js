/**
 * Tests that when featureFlagUniqueShardIdentifiers is enabled, the addShard and
 * transitionFromDedicatedConfigServer operations persist a unique uuid in config.shards and
 * propagate it into the shard's identity document on the newly added shard.
 *
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagUniqueShardIdentifiers,
 *   config_shard_incompatible,
 *   # This test calls addShard manually and checks sharding metadata.
 *   assumes_stable_shard_list,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/** Return whether the value is a UUID string (36-char canonical form). */
function isUUIDString(val) {
    return typeof val === "string" && /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(val);
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
    assert(uuid instanceof BinData && uuid.subtype() === 4, "expected uuid to be a UUID BinData", {uuid});

    // BinData.toString() returns "UUID(\"<canonical string>\")" in the shell.
    const uuidStr = uuid.toString().replace(/^UUID\("(.+)"\)$/, "$1");
    assert(isUUIDString(uuidStr), "expected a canonical UUID string", {uuidStr});
    return uuidStr;
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
    const identityDoc = rs.getPrimary().getDB("admin").system.version.findOne({_id: "shardIdentity"});
    assert(identityDoc, "expected shardIdentity document on the added shard");
    assert(
        identityDoc.uuid instanceof BinData && identityDoc.uuid.subtype() === 4,
        "expected uuid in shardIdentity to be a UUID BinData",
        {uuid: identityDoc.uuid},
    );

    const identityUuidStr = identityDoc.uuid.toString().replace(/^UUID\("(.+)"\)$/, "$1");
    assert.eq(configUuidStr, identityUuidStr, "uuid in config.shards and shardIdentity on the shard must match");

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
    const identityDoc = st.configRS.getPrimary().getDB("admin").system.version.findOne({_id: "shardIdentity"});
    assert(identityDoc, "expected shardIdentity document on the config server");
    assert(identityDoc.uuid !== undefined, "expected uuid field to be set in shardIdentity", {
        identityDoc,
    });

    const uuid = identityDoc.uuid;
    assert(uuid instanceof BinData && uuid.subtype() === 4, "expected uuid to be a UUID BinData", {uuid});
    const uuidStr = uuid.toString().replace(/^UUID\("(.+)"\)$/, "$1");

    assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));

    // Verify config.shards carries a UUID and it matches the one in the shard identity.
    const configUuidStr = assertAndGetShardUuid(configDb, "config");
    assert.eq(uuidStr, configUuidStr, "uuid in shardIdentity and config.shards must match");

    st.stop();
}
