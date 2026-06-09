/**
 * Tests that setFeatureCompatibilityVersion correctly populates 'uuid' fields
 * across config.shards and shard identity documents.
 * TODO SERVER-126212 Remove this file once 9.0 becomes last LTS.
 *
 * @tags: [
 *   featureFlagUniqueShardIdentifiers,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("FCV upgrade/downgrade uuid fields", function () {
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

    function setupForFCVUpgradeTest(clearExistingUuids) {
        // Ensure that the upgrade request won't result into a no-op.
        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
        checkIndexOnShardUuid(false /* expectedToBePresent */);
        // Clear pre-existing metadata if requested.
        if (clearExistingUuids) {
            const shardsInFixture = getShards();
            // Clear uuid values from config.shards docs.
            const updateRes = assert.commandWorked(st.s.getDB("config").shards.updateMany({}, {$unset: {uuid: 1}}));
            assert.eq(shardsInFixture.length, updateRes.modifiedCount);
            // Clear uuid values from all shardIdentity docs.
            assert.commandWorked(
                st.configRS
                    .getPrimary()
                    .getDB("admin")
                    .system.version.updateOne({_id: "shardIdentity"}, {$unset: {uuid: 1}}),
            );
            for (const shard of shardsInFixture) {
                assert.commandWorked(
                    shard.getDB("admin").system.version.updateOne({_id: "shardIdentity"}, {$unset: {uuid: 1}}),
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

    /* Checks that each shard's shardIdentity document contains a 'uuid' field that matches the corresponding config.shards doc. */
    function assertShardIdentityDocsHaveConsistentUuids() {
        for (const shard of getShards()) {
            const identityDoc = shard.getDB("admin").system.version.findOne({_id: "shardIdentity"});
            const configShardsDoc = st.s.getDB("config").shards.findOne({_id: shard.shardName});
            assert.neq(null, identityDoc, `shardIdentity document must exist on node ${shard.host}`);
            assert.eq(
                identityDoc.uuid,
                configShardsDoc.uuid,
                `shardIdentity on ${shard.host}: uuid '${tojsononeline(identityDoc.uuid)}' must equal config.shards uuid '${tojsononeline(configShardsDoc.uuid)}'`,
            );
        }

        // The shardIdentity doc on the config server should also have a uuid field.
        const configServer = st.configRS.getPrimary();
        const configServerIdentityDoc = configServer.getDB("admin").system.version.findOne({_id: "shardIdentity"});
        assertIsUUID(configServerIdentityDoc.uuid);
    }

    it("[Dedicated CSRS] Should set consistent metadata on FCV upgrade", function () {
        setupForFCVUpgradeTest(true /* clearExistingUuids */);
        const topologyTimeBeforeUpgrade = getMaxTopologyTimeInConfigShards();
        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
        const topologyTimeAfterFirstUpgrade = getMaxTopologyTimeInConfigShards();
        assert(
            timestampCmp(topologyTimeAfterFirstUpgrade, topologyTimeBeforeUpgrade) > 0,
            `Expected topologyTime to be bumped on FCV upgrade: ${tojsononeline(topologyTimeBeforeUpgrade)} -> ${tojsononeline(topologyTimeAfterFirstUpgrade)}`,
        );

        const generatedUUIDsOnFirstInvocation = assertConfigShardsHaveUuids();
        assertShardIdentityDocsHaveConsistentUuids();

        checkIndexOnShardUuid(true /* expectedToBePresent */);

        // Repeat the upgrade to verify idempotency.
        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

        const topologyTimeAfterNoopUpgrade = getMaxTopologyTimeInConfigShards();
        assert(
            timestampCmp(topologyTimeAfterNoopUpgrade, topologyTimeAfterFirstUpgrade) === 0,
            `Expected topologyTime to be preserved on no-op FCV upgrade: ${tojsononeline(topologyTimeAfterFirstUpgrade)} -> ${tojsononeline(topologyTimeAfterNoopUpgrade)}`,
        );

        assertConfigShardsHaveUuids(generatedUUIDsOnFirstInvocation);
        assertShardIdentityDocsHaveConsistentUuids();
        checkIndexOnShardUuid(true /* expectedToBePresent */);
    });

    it("[Dedicated CSRS] Should keep pre-existing uuid values in config.shards during FCV upgrade", function () {
        setupForFCVUpgradeTest(true /* clearExistingUuids */);
        // Inject uuid values into config.shards docs.
        // These are expected to be retained once the cluster will be later FCV upgraded.
        const expectedValuesByShardName = {};
        getShards().forEach((shard) => {
            const uuid = UUID();
            expectedValuesByShardName[shard.shardName] = uuid;
            assert.commandWorked(st.s.getDB("config").shards.updateOne({_id: shard.shardName}, {$set: {uuid: uuid}}));
        });

        // Inject bogus uuid values into all shardIdentity docs.
        // These are expected to be overwritten with the correct values from config.shards during the FCV upgrade.
        const bogusUUID = UUID();

        for (const shard of getShards()) {
            assert.commandWorked(
                shard.getDB("admin").system.version.updateOne({_id: "shardIdentity"}, {$set: {uuid: bogusUUID}}),
            );
        }

        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

        // Verify bogus values have been replaced with the correct shard identifiers.
        assertConfigShardsHaveUuids(expectedValuesByShardName);
        assertShardIdentityDocsHaveConsistentUuids();
        checkIndexOnShardUuid(true /* expectedToBePresent */);
    });

    it("[Dedicated CSRS] Should drop the uuid_1 index of config.shards on FCV downgrade", function () {
        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
        checkIndexOnShardUuid(true /* expectedToBePresent */);
        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
        checkIndexOnShardUuid(false /* expectedToBePresent */);
    });
});
