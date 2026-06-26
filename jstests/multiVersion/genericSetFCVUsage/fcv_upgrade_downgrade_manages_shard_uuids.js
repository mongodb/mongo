/**
 * Tests that setFeatureCompatibilityVersion correctly populates 'uuid' fields
 * across config.shards and shard identity documents, including the fixed config
 * server UUID defined in ShardHandle::kConfigServerHandle.
 * TODO SERVER-126212 Remove this file once 9.0 becomes last LTS.
 *
 * @tags: [
 *   featureFlagUniqueShardIdentifiersDDL,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("[Dedicated CSRS] FCV upgrade/downgrade uuid fields", function () {
    // Must match ShardHandle::kConfigServerHandle in shard_handle.cpp.
    const kConfigServerUuid = UUID("636f6e66-6967-4000-8000-000000000000");

    let st;

    before(function () {
        st = new ShardingTest({shards: 3, mongos: 1, rs: {nodes: 1}});

        // At startup no shard document carries a uuid field yet, so createIndexForConfigShards
        // must skip index creation. Verify that no index on the uuid key exists.
        checkIndexOnShardUuid(false /* expectedToBePresent */);
    });

    after(function () {
        st.stop();
    });

    function verifyShardMetadataOnShardingState(connToShard, expectedShardId, expectedUuid) {
        const shardingState = assert.commandWorked(connToShard.adminCommand({shardingState: 1}));
        assert(shardingState.enabled, "shardingState command should indicate sharding is enabled");
        assert.eq(
            shardingState.shardName,
            expectedShardId,
            `expected shardingState shardName to be ${expectedShardId}, found ${shardingState.shardName}`,
        );
        assert.eq(
            shardingState.shardUuid,
            expectedUuid,
            `expected shardingState shardUuid to be ${tojsononeline(expectedUuid)}, found ${tojsononeline(shardingState.shardUuid)}`,
        );
    }

    function setupForFCVUpgradeTest(clearExistingUuids) {
        // Ensure that the upgrade request won't result into a no-op.
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        checkIndexOnShardUuid(false /* expectedToBePresent */);
        // Clear pre-existing metadata if requested.
        if (clearExistingUuids) {
            const shardsInFixture = getShards();
            // Clear uuid values from config.shards docs.
            const updateRes = assert.commandWorked(
                st.s.getDB("config").shards.updateMany({}, {$unset: {uuid: 1}}),
            );
            assert.eq(shardsInFixture.length, updateRes.modifiedCount);
            // Clear uuid values from all shardIdentity docs. The cleanup should also be visible through the shardingState command.
            assert.commandWorked(
                st.configRS
                    .getPrimary()
                    .getDB("admin")
                    .system.version.updateOne({_id: "shardIdentity"}, {$unset: {uuid: 1}}),
            );

            verifyShardMetadataOnShardingState(st.configRS.getPrimary(), "config", undefined);

            for (const shard of shardsInFixture) {
                assert.commandWorked(
                    shard
                        .getDB("admin")
                        .system.version.updateOne({_id: "shardIdentity"}, {$unset: {uuid: 1}}),
                );

                verifyShardMetadataOnShardingState(
                    shard.rs.getPrimary(),
                    shard.shardName,
                    undefined,
                );
            }
        }
    }

    function getShards() {
        return [st.shard0, st.shard1, st.shard2];
    }

    function getMaxTopologyTimeInConfigShards() {
        const result = st.s
            .getDB("config")
            .shards.aggregate([{$group: {_id: null, maxTopologyTime: {$max: "$topologyTime"}}}])
            .toArray();
        return result.length > 0 ? result[0].maxTopologyTime : null;
    }

    function checkIndexOnShardUuid(expectedToBePresent) {
        const indexes = st.s.getDB("config").shards.getIndexes();
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

    function assertIsUUID(bsonFieldValue) {
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

    /* Checks for the existence of 'uuid' fields in config.shards documents and optionally verifies that they match expected values.
     * Returns a dictionary of retrieved UUIDs keyed by shard name.
     */
    function assertConfigShardsHaveUuids(expectedValuesByShardName = null) {
        const shardDocs = st.s.getDB("config").shards.find({}).toArray();
        const shardsInFixture = getShards();
        assert.eq(shardDocs.length, shardsInFixture.length);
        const uniqueUuidsInConfigShards = new Set();

        for (const shardDoc of shardDocs) {
            assert(
                shardDoc.uuid !== undefined,
                `expected uuid field to be set in config.shards doc ${tojsononeline(shardDoc)}`,
            );

            assertIsUUID(shardDoc.uuid);
            uniqueUuidsInConfigShards.add(tojsononeline(shardDoc.uuid));

            if (expectedValuesByShardName) {
                const expectedUuid = expectedValuesByShardName[shardDoc._id];
                assert.eq(
                    shardDoc.uuid,
                    expectedUuid,
                    `expected uuid for shard ${shardDoc._id} to be ${tojsononeline(expectedUuid)}, found ${tojsononeline(shardDoc.uuid)}`,
                );
            }
        }

        assert.eq(
            uniqueUuidsInConfigShards.size,
            getShards().length,
            `expected all config.shards uuids to be unique, found ${tojsononeline(shardDocs)}`,
        );
        return shardDocs.reduce((acc, shardDoc) => {
            acc[shardDoc._id] = shardDoc.uuid;
            return acc;
        }, {});
    }

    function assertConfigServerHasFixedUuid() {
        const configServer = st.configRS.getPrimary();
        const identityDoc = configServer
            .getDB("admin")
            .system.version.findOne({_id: "shardIdentity"});
        assert.neq(null, identityDoc, "shardIdentity document must exist on config server");
        assertIsUUID(identityDoc.uuid);
        assert.eq(
            identityDoc.uuid,
            kConfigServerUuid,
            "config server shardIdentity uuid must match ShardHandle::kConfigServerHandle",
        );
        verifyShardMetadataOnShardingState(configServer, "config", kConfigServerUuid);
    }

    // Checks that each shard's shardIdentity document contains a 'uuid' field that matches the corresponding config.shards doc.
    // The same value should be visible through the shardingState command on each shard.
    function assertShardIdentityDocsHaveConsistentUuids() {
        for (const shard of getShards()) {
            const identityDoc = shard.getDB("admin").system.version.findOne({_id: "shardIdentity"});
            const configShardsDoc = st.s.getDB("config").shards.findOne({_id: shard.shardName});
            assert.neq(
                null,
                identityDoc,
                `shardIdentity document must exist on node ${shard.host}`,
            );
            assert.eq(
                identityDoc.uuid,
                configShardsDoc.uuid,
                `shardIdentity on ${shard.host}: uuid '${tojsononeline(identityDoc.uuid)}' must equal config.shards uuid '${tojsononeline(configShardsDoc.uuid)}'`,
            );
            verifyShardMetadataOnShardingState(
                shard.rs.getPrimary(),
                shard.shardName,
                identityDoc.uuid,
            );
        }

        assertConfigServerHasFixedUuid();
    }

    it("Should set consistent metadata in 'config.shards' on FCV upgrade", function () {
        setupForFCVUpgradeTest(true /* clearExistingUuids */);
        const topologyTimeBeforeUpgrade = getMaxTopologyTimeInConfigShards();
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        const topologyTimeAfterFirstUpgrade = getMaxTopologyTimeInConfigShards();
        assert(
            timestampCmp(topologyTimeAfterFirstUpgrade, topologyTimeBeforeUpgrade) > 0,
            `Expected topologyTime to be bumped on FCV upgrade: ${tojsononeline(topologyTimeBeforeUpgrade)} -> ${tojsononeline(topologyTimeAfterFirstUpgrade)}`,
        );

        const generatedUUIDsOnFirstInvocation = assertConfigShardsHaveUuids();
        assertShardIdentityDocsHaveConsistentUuids();

        checkIndexOnShardUuid(true /* expectedToBePresent */);

        // Repeat the upgrade to verify idempotency.
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );

        const topologyTimeAfterNoopUpgrade = getMaxTopologyTimeInConfigShards();
        assert(
            timestampCmp(topologyTimeAfterNoopUpgrade, topologyTimeAfterFirstUpgrade) === 0,
            `Expected topologyTime to be preserved on no-op FCV upgrade: ${tojsononeline(topologyTimeAfterFirstUpgrade)} -> ${tojsononeline(topologyTimeAfterNoopUpgrade)}`,
        );

        assertConfigShardsHaveUuids(generatedUUIDsOnFirstInvocation);
        assertShardIdentityDocsHaveConsistentUuids();
        checkIndexOnShardUuid(true /* expectedToBePresent */);
    });

    it("Pre-existing uuid values in 'config.shards' should be preserved during an FCV downgrade/upgrade cycle", function () {
        setupForFCVUpgradeTest(true /* clearExistingUuids */);

        // Perform a first upgrade to set uuid fields for each shard.
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        const generatedUUIDsOnFirstUpgrade = assertConfigShardsHaveUuids();
        assertConfigServerHasFixedUuid();

        // Downgrade preserves the uuid fields (while dropping the related index).
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        assertConfigShardsHaveUuids(generatedUUIDsOnFirstUpgrade);
        assertConfigServerHasFixedUuid();
        checkIndexOnShardUuid(false /* expectedToBePresent */);

        // Verify the expected state upon a re-upgrade.
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );

        assertConfigShardsHaveUuids(generatedUUIDsOnFirstUpgrade);
        assertShardIdentityDocsHaveConsistentUuids();
        checkIndexOnShardUuid(true /* expectedToBePresent */);
    });

    it("Should drop the uuid_1 index of config.shards on FCV downgrade", function () {
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        checkIndexOnShardUuid(true /* expectedToBePresent */);
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        checkIndexOnShardUuid(false /* expectedToBePresent */);
    });

    // TODO SERVER-129221 Re-enable test case.
    it.skip("Should set consistent UUIDs in shardRef fields of the global catalog on FCV upgrade", function () {
        const dbName = "testDB";
        const collName = "coll";
        const nss = `${dbName}.${collName}`;

        setupForFCVUpgradeTest(true /* clearExistingUuids */);

        // Transition to embedded config server to also cover how its shardRef is updated during the upgrade.
        assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));

        let shardByName = {};
        for (const shardDoc of st.s.getDB("config").shards.find({}).toArray()) {
            shardByName[shardDoc._id] = shardDoc;
        }

        const primaryShardName = (() => {
            const randomShardIdx = Math.floor(Math.random() * Object.keys(shardByName).length);
            let visited = 0;
            for (let shardId of Object.keys(shardByName)) {
                if (visited === randomShardIdx) {
                    return shardId;
                }
                visited++;
            }
        })();

        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}),
        );

        const dbDocBefore = st.s.getDB("config").databases.findOne({_id: dbName});
        assert.neq(null, dbDocBefore, `${dbName} should appear in config.databases`);
        assert.eq(
            primaryShardName,
            dbDocBefore.primary,
            `config.databases.primary should be the shard name before FCV upgrade`,
        );

        assert.commandWorked(
            st.s.adminCommand({
                shardCollection: nss,
                key: {_id: "hashed"},
                numInitialChunks: 4,
            }),
        );

        const collUuid = st.s.getDB("config").collections.findOne({_id: nss}).uuid;
        const chunksBefore = st.s.getDB("config").chunks.find({uuid: collUuid}).toArray();
        for (const chunk of chunksBefore) {
            assert(
                shardByName[chunk.shard] !== undefined,
                `chunk shard should match a config.shards _id before FCV upgrade for ${nss}`,
                {chunk},
            );
        }

        // Record shard assignments before upgrade for precise post-upgrade verification.
        const chunkIdToParentShardId = {};
        for (const chunk of chunksBefore) {
            chunkIdToParentShardId[tojsononeline(chunk._id)] = chunk.shard;
        }

        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );

        // Recompute the mapping after FCV upgrade.
        shardByName = {};
        for (const shardDoc of st.s.getDB("config").shards.find({}).toArray()) {
            shardByName[shardDoc._id] = shardDoc;
        }

        // Assert config.databases.primary for testDB is now the UUID of its primary shard.
        const dbDocAfter = st.s.getDB("config").databases.findOne({_id: dbName});
        assert.neq(
            null,
            dbDocAfter,
            `${dbName} should still appear in config.databases after FCV upgrade`,
        );
        const expectedDbUuid = shardByName[primaryShardName].uuid;
        assert.neq(
            undefined,
            expectedDbUuid,
            `config.shards should have a uuid for shard ${primaryShardName} after FCV upgrade`,
        );
        assertIsUUID(dbDocAfter.primary);
        assert.eq(
            expectedDbUuid,
            dbDocAfter.primary,
            `config.databases.primary should be updated to the shard UUID after FCV upgrade for ${dbName}`,
        );

        // Assert each config.chunks shard field for testDB.testColl is now the UUID of its shard.
        const chunksAfter = st.s.getDB("config").chunks.find({uuid: collUuid}).toArray();
        assert.eq(4, chunksAfter.length, `expected 4 chunks for ${nss} after FCV upgrade`, {
            chunksAfter,
        });
        for (const chunk of chunksAfter) {
            const originalShardName = chunkIdToParentShardId[tojsononeline(chunk._id)];
            const expectedChunkUuid = shardByName[originalShardName].uuid;
            assertIsUUID(chunk.shard);
            assert.eq(
                expectedChunkUuid,
                chunk.shard,
                `chunk shard should be updated to UUID of shard ${originalShardName} after FCV upgrade for ${nss}`,
            );
        }

        // Drop the converted database to ensure proper test teardown
        assert.commandWorked(st.s.getDB(dbName).dropDatabase());
    });
});
