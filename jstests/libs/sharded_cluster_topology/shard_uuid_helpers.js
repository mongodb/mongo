/**
 * Helpers for inspecting shard UUID metadata (config.shards indexes/documents and shardIdentity
 * documents).
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";

function _checkIndexOnShardUuid(configDB, expectedToBePresent) {
    const indexes = configDB.shards.getIndexes();
    if (expectedToBePresent) {
        assert(
            indexes.some((idx) => idx.key.uuid !== undefined),
            "expected a uuid index on config.shards",
            {indexes},
        );
    } else {
        assert(
            indexes.every((idx) => idx.key.uuid === undefined),
            "expected no uuid index on config.shards",
            {indexes},
        );
    }
}

function _assertIsUUID(bsonFieldValue) {
    assert(
        bsonFieldValue instanceof BinData && bsonFieldValue.subtype() === 4,
        `expected uuid to be a UUID BinData ${tojsononeline(bsonFieldValue)}`,
    );
    // BinData.toString() returns "UUID(\"<canonical string>\")" in the shell.
    const uuidStr = bsonFieldValue.toString().replace(/^UUID\("(.+)"\)$/, "$1");
    const isCanonicalUuidString =
        typeof uuidStr === "string" &&
        /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(uuidStr);
    assert(
        isCanonicalUuidString,
        `expected a canonical UUID string: ${tojsononeline(bsonFieldValue)} -> ${uuidStr}`,
    );
}

/*
 * Checks the presence/absence 'uuid' field in the shardIdentity document of each shard provided.
 * - Each element of the 'connToShards' array is expected to be a connection to a shard's (or config server) primary node.
 * -  When 'expectedToExist' is true, the function returns a map of shardId to uuid.
 */
function _checkUuidsInShardIdentityDocs(connToShards, expectedToExist) {
    const uuidsByShardId = {};
    for (const conn of connToShards) {
        const identityDoc = conn.getDB("admin").system.version.findOne({_id: "shardIdentity"});
        assert.neq(null, identityDoc, `shardIdentity document must exist on node ${conn.host}`);
        if (expectedToExist) {
            _assertIsUUID(identityDoc.uuid);
            uuidsByShardId[identityDoc.shardName] = identityDoc.uuid;
        } else {
            assert(
                !identityDoc.hasOwnProperty("uuid"),
                `expected no uuid field in shardIdentity document on ${conn.host}`,
                {identityDoc},
            );
        }
    }
    return expectedToExist ? uuidsByShardId : null;
}

// Hardcoded, well-known UUID assigned to the config server. Must match
// ShardType::kConfigServerUuid on the server side. The UUID is the word "config" in hex
// (636f6e66-6967) plus the v4 required bits.
const kConfigServerUuid = UUID("636f6e66-6967-4000-8000-000000000000");

/*
 * Based on the received value of 'expectedToExist' (which defaults to the activation state of featureFlagAssignUUIDToShard),
 *  asserts that the shard UUID metadata of the cluster is consistent by verifying that:
 * - each config.shards document includes/excludes the 'uuid' field of the expected type;
 * - the secondary index of config.shards on 'uuid' is present/absent;
 * - the shardIdentity document of each shard is consistent with the config.shards document for that
 *   shard;
 * - the config server carries the well-known 'kConfigServerUuid' value. A dedicated config server
 *   has no config.shards document, so its shardIdentity document is verified directly.
 *
 * 'db' is expected to be a connection to a mongos (or a router-enabled node) of a sharded cluster.
 * On a non-sharded deployment the function is a no-op.
 * TODO SERVER-126212 Consider whether this method needs to be removed once 9.0 becomes last LTS.
 */
function assertShardUuidMetadataConsistency(db, expectedToExist = null) {
    if (expectedToExist === null) {
        expectedToExist = FeatureFlagUtil.isPresentAndEnabled(db, "AssignUUIDToShard");
    }
    // Nothing to verify if not testing a sharded cluster.
    const listShardsRes = db.adminCommand({listShards: 1});
    if (listShardsRes.code === ErrorCodes.CommandNotFound) {
        return;
    }
    assert.commandWorked(listShardsRes);

    const configDB =
        typeof db.getDB === "function" ? db.getDB("config") : db.getSiblingDB("config");

    _checkIndexOnShardUuid(configDB, expectedToExist);

    // Verify each config.shards document and the shardIdentity document of the corresponding shard.
    let embeddedConfigServerDetected = false;
    for (const shardDoc of configDB.shards.find().toArray()) {
        if (expectedToExist) {
            _assertIsUUID(shardDoc.uuid);
            if (shardDoc._id === "config") {
                assert.eq(
                    shardDoc.uuid,
                    kConfigServerUuid,
                    "config server shard must carry the well-known config server UUID",
                    {shardDoc},
                );
            }
        } else {
            assert(
                !shardDoc.hasOwnProperty("uuid"),
                `expected no uuid field in config.shards doc for ${shardDoc._id}`,
                {shardDoc},
            );
        }

        if (shardDoc._id === "config") {
            embeddedConfigServerDetected = true;
        }

        const shardConn = newMongoWithRetry(shardDoc.host, undefined, {gRPC: false});
        try {
            const uuidsByShardId = _checkUuidsInShardIdentityDocs([shardConn], expectedToExist);
            if (expectedToExist) {
                assert.eq(
                    uuidsByShardId[shardDoc._id],
                    shardDoc.uuid,
                    `shardIdentity uuid on ${shardDoc._id} must match its config.shards uuid`,
                    {shardDoc},
                );
            }
        } finally {
            shardConn.close();
        }
    }

    // A dedicated config server has no config.shards document, so it was not visited by the loop
    // above. Verify its shardIdentity document directly to confirm it carries (or lacks) the
    // well-known config server UUID.
    if (!embeddedConfigServerDetected) {
        const configConnStr = assert.commandWorked(db.adminCommand({getShardMap: 1})).map.config;
        const configConn = newMongoWithRetry(configConnStr, undefined, {gRPC: false});
        try {
            const uuidsByShardId = _checkUuidsInShardIdentityDocs([configConn], expectedToExist);
            if (expectedToExist) {
                assert.eq(
                    uuidsByShardId["config"],
                    kConfigServerUuid,
                    "config server shardIdentity must carry the well-known config server UUID",
                );
            }
        } finally {
            configConn.close();
        }
    }
}

export {kConfigServerUuid, assertShardUuidMetadataConsistency};
