/*
 * SERVER-125698: checkMetadataConsistency must walk the DBPrimary shard's durable shard catalog
 * even when the DBPrimary owns no chunks for a sharded collection.
 *
 * In an authoritative world (featureFlagAuthoritativeShardsCRUD), the DBPrimary shard must always
 * carry a chunk entry in its durable shard catalog (config.shard.catalog.chunks) for every
 * tracked collection it is primary for. If it owns no user chunks, that entry is a chunkless
 * placeholder. Pre-fix, the consistency check short-circuited on a {0,0} placement version and
 * never inspected the DBPrimary's durable shard catalog -- letting drift between
 * config.shard.catalog.chunks and the global catalog go undetected. Post-fix, the durable
 * shard catalog is walked unconditionally on the DBPrimary, and a missing placeholder surfaces
 * as InconsistentShardCatalogCollectionMetadata.
 *
 * @tags: [
 *   requires_fcv_70,
 *   featureFlagAuthoritativeShardsCRUD,
 *   # TODO SERVER-124153: Revisit this tag.
 *   featureFlagReplicatedFastCount_incompatible,
 *   # Drops the cluster-managed shard catalog directly; balancer-driven migrations during the
 *   # window between the drop and the consistency check would race with the assertions.
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, other: {enableBalancer: false}});
const mongos = st.s;

if (!FeatureFlagUtil.isPresentAndEnabled(st.s, "AuthoritativeShardsCRUD")) {
    jsTestLog(
        "Skipping test: featureFlagAuthoritativeShardsCRUD must be enabled for the DBPrimary " +
            "durable shard catalog check to apply.",
    );
    st.stop();
    quit();
}

const kShardCatalogChunksNs = "shard.catalog.chunks";
const kShardCatalogCollNs = "shard.catalog.collections";

const primaryShard = st.shard0;
const recipientShard = st.shard1;

let dbCounter = 0;
function getNewDb() {
    return mongos.getDB(jsTestName() + "_" + dbCounter++);
}

function getCollUuid(db, collName) {
    return db.getCollectionInfos({name: collName})[0].info.uuid;
}

function getShardCatalogChunks(shardConn, uuid) {
    return shardConn.getDB("config").getCollection(kShardCatalogChunksNs).find({uuid: uuid}).toArray();
}

function checkMetadataInconsistencies(db) {
    return db.checkMetadataConsistency().toArray();
}

function assertNoInconsistencies(db) {
    const res = checkMetadataInconsistencies(db);
    assert.eq(0, res.length, "Expected no inconsistencies but found: " + tojson(res));
}

function moveAllChunksOff(ns, fromShard, toShard, uuid) {
    const chunks = mongos.getDB("config").chunks.find({uuid: uuid, shard: fromShard.shardName}).toArray();
    assert.gt(chunks.length, 0, "Setup invariant: source shard must own at least one chunk");
    chunks.forEach((chunk) => {
        assert.commandWorked(
            mongos.adminCommand({moveChunk: ns, find: chunk.min, to: toShard.shardName}),
        );
    });
}

// ------------------------------------------------------------------------------------------------
// Baseline: DBPrimary with a chunkless-placeholder entry is consistent.
// Establishes that the post-fix walk does not produce false positives in the steady state.
// ------------------------------------------------------------------------------------------------
(function testDbPrimaryChunklessPlaceholderIsConsistent() {
    jsTest.log("Executing testDbPrimaryChunklessPlaceholderIsConsistent");

    const db = getNewDb();
    const collName = "coll";
    const ns = db.getName() + "." + collName;

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: primaryShard.shardName}),
    );
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {x: 1}}));

    const uuid = getCollUuid(db, collName);
    moveAllChunksOff(ns, primaryShard, recipientShard, uuid);

    // Sanity: DBPrimary owns no global-catalog chunks for this collection.
    assert.eq(
        0,
        mongos.getDB("config").chunks.countDocuments({uuid: uuid, shard: primaryShard.shardName}),
        "Setup invariant: DBPrimary should own no global-catalog chunks after migration",
    );

    // The DBPrimary's durable shard catalog must still carry a (placeholder) entry for the
    // collection; the consistency check should walk it and report nothing.
    assertNoInconsistencies(db);

    db.dropDatabase();
    assertNoInconsistencies(mongos.getDB("admin"));
})();

// ------------------------------------------------------------------------------------------------
// Drift: DBPrimary chunkless-placeholder entry is missing from config.shard.catalog.chunks.
// Pre-fix: silently passed (DBPrimary skipped because it owned no chunks).
// Post-fix: surfaces InconsistentShardCatalogCollectionMetadata.
// ------------------------------------------------------------------------------------------------
(function testDbPrimaryMissingChunklessPlaceholderIsCaught() {
    jsTest.log("Executing testDbPrimaryMissingChunklessPlaceholderIsCaught");

    const db = getNewDb();
    const collName = "coll";
    const ns = db.getName() + "." + collName;

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: primaryShard.shardName}),
    );
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {x: 1}}));

    const uuid = getCollUuid(db, collName);
    moveAllChunksOff(ns, primaryShard, recipientShard, uuid);

    // Pre-condition: the placeholder entry exists on the DBPrimary durable shard catalog.
    const beforeChunks = getShardCatalogChunks(primaryShard, uuid);
    assert.gte(
        beforeChunks.length,
        1,
        "Setup invariant: DBPrimary durable shard catalog must hold at least a placeholder chunk " +
            "entry for the tracked collection",
    );

    // Simulate drift: drop the DBPrimary's durable shard catalog chunk entries for this UUID.
    // This is the targeted state the SERVER-125698 fix must detect.
    assert.commandWorked(
        primaryShard
            .getDB("config")
            .getCollection(kShardCatalogChunksNs)
            .runCommand({delete: kShardCatalogChunksNs, deletes: [{q: {uuid: uuid}, limit: 0}]}),
    );

    // Verify the drift was applied locally.
    assert.eq(
        0,
        getShardCatalogChunks(primaryShard, uuid).length,
        "Pre-check invariant: DBPrimary durable shard catalog chunks should be empty for drift " +
            "scenario",
    );

    // Database-level check should observe the DBPrimary-side drift. The inconsistency identifies
    // the affected collection via the wrapper's namespace/collectionUUID, and the inner details
    // object carries a 'reason' string from getChunksFromDurableShardCatalog when the durable
    // chunk list is empty.
    const dbInconsistencies = checkMetadataInconsistencies(db);
    const matchesDriftForUuid = (object) =>
        object.type === "InconsistentShardCatalogCollectionMetadata" &&
        object.details &&
        bsonWoCompare(object.details.collectionUUID, uuid) === 0;
    assert(
        dbInconsistencies.some(matchesDriftForUuid),
        "Expected InconsistentShardCatalogCollectionMetadata for collection UUID " + uuid +
            ", got: " + tojson(dbInconsistencies),
    );

    // Collection-level check must surface the same inconsistency (DBPrimary participates even
    // though it owns zero chunks).
    const collInconsistencies = db.getCollection(collName).checkMetadataConsistency().toArray();
    assert(
        collInconsistencies.some(matchesDriftForUuid),
        "Collection-level check must also raise the DBPrimary-side inconsistency, got: " +
            tojson(collInconsistencies),
    );

    // Tear down without re-asserting absence -- the durable shard catalog drift is repaired by
    // dropDatabase, which clears the global-catalog entry and recreates a consistent state.
    db.dropDatabase();
    assertNoInconsistencies(mongos.getDB("admin"));
})();

// ------------------------------------------------------------------------------------------------
// Drift: DBPrimary durable shard catalog collection metadata is missing while chunks exist
// elsewhere. Exercises that the DBPrimary check is not gated on "owns at least one chunk".
// ------------------------------------------------------------------------------------------------
(function testDbPrimaryMissingShardCatalogCollectionDocIsCaught() {
    jsTest.log("Executing testDbPrimaryMissingShardCatalogCollectionDocIsCaught");

    const db = getNewDb();
    const collName = "coll";
    const ns = db.getName() + "." + collName;

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: primaryShard.shardName}),
    );
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {x: 1}}));

    const uuid = getCollUuid(db, collName);
    moveAllChunksOff(ns, primaryShard, recipientShard, uuid);

    // Drop the DBPrimary's shard.catalog.collections entry for this nss. Pre-fix this was not
    // observed because the DBPrimary was skipped on the {0,0} placement-version short-circuit.
    assert.commandWorked(
        primaryShard.getDB("config").getCollection(kShardCatalogCollNs).runCommand({
            delete: kShardCatalogCollNs,
            deletes: [{q: {_id: ns}, limit: 0}],
        }),
    );

    const inconsistencies = checkMetadataInconsistencies(db);
    assert(
        inconsistencies.some(
            (object) =>
                object.type === "InconsistentShardCatalogCollectionMetadata" &&
                object.details &&
                bsonWoCompare(object.details.collectionUUID, uuid) === 0,
        ),
        "Expected InconsistentShardCatalogCollectionMetadata for missing DBPrimary " +
            "shard.catalog.collections entry, got: " + tojson(inconsistencies),
    );

    db.dropDatabase();
    assertNoInconsistencies(mongos.getDB("admin"));
})();

st.stop();
