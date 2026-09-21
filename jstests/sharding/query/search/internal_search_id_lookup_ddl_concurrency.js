/**
 * Tests that `$_internalSearchIdLookup` correctly drops orphaned documents left behind by a chunk
 * migration, across sharding strategies, `_id` shapes, and DDL timing relative to getMores.
 *
 * @tags: [requires_fcv_90]
 */
import {ClusteredCollectionUtil} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {withFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ServerStatusMetrics} from "jstests/libs/query/change_stream_metrics_util.js";
import {assertCollDataDistribution} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";
import {
    createInternalDB,
    mockMongotPipeline,
    readIdLookupDelta,
    runAggWithMockMongot,
    runAggWithMockMongotResults,
} from "jstests/libs/query/internal_search_id_lookup_util.js";
import {
    withClusteredColl,
    withShardedColl,
} from "jstests/libs/query/collection_config_decorators.js";

// Same config/decorator shape as the sharded change-stream updateLookup matrix and the
// $_internalSearchIdLookup aggregation tests: 'key' is the shard-key spec, 'collOpts' the
// collection options, and the shared decorators add the clustered and hashed/range variants.

// _id-keyed sharding ties the shard key's value to _id itself, and its compound cell pins the
// SERVER-134686 known issue (see reportsOrphanAsFound). sk-keyed sharding uses a separate shard
// key field, so _id is free to vary independently, crossed with both id shapes.
const shardKeyConfigs = [
    {name: "_id-keyed, scalar _id", key: {_id: 1}, makeId: (seed) => seed, collOpts: {}},
    {name: "sk-keyed, scalar _id", key: {sk: 1}, makeId: (seed) => seed, collOpts: {}},
    {
        name: "sk-keyed, compound _id",
        key: {sk: 1},
        makeId: (seed) => ({a: seed, b: seed % 3}),
        collOpts: {},
    },
    {
        name: "_id-keyed, compound _id",
        key: {_id: 1},
        makeId: (seed) => ({a: seed, b: seed % 3}),
        collOpts: {},
    },
]
    .flatMap((config) => [config, withClusteredColl(config)])
    .flatMap(withShardedColl);

function makeDoc(config, seed) {
    const shardKeyField = Object.keys(config.key)[0];
    const doc = {_id: config.makeId(seed)};
    if (shardKeyField !== "_id") {
        doc[shardKeyField] = seed;
    }
    return doc;
}

function shardKeyFilter(config, seed) {
    const shardKeyField = Object.keys(config.key)[0];
    return shardKeyField === "_id" ? {_id: config.makeId(seed)} : {[shardKeyField]: seed};
}

describe("$_internalSearchIdLookup sharding/DDL concurrency", function () {
    let st;
    let mongosDB;
    let internalDB0;
    let internalDB1;

    // Whether SBE can encode this config's _id shape.
    //
    // TODO: SERVER-134080 Support non-scalar _id lookups in SbeSingleDocumentLookupExecutor.
    // Until then, an object _id is only encodable on a clustered collection, and the local-read
    // fallback handles it instead.
    //
    // 'allCollectionsClusteredByDefault' is set in before(): the sharding_clustered_collections
    // suite runs with the 'clusterAllCollectionsByDefault' failpoint, which clusters every
    // collection on _id at creation regardless of collOpts — including the collOpts:{} configs
    // below — making compound _id encodable there too. The lookups run shard-local, so the
    // failpoint is read from a shard, not mongos.
    let allCollectionsClusteredByDefault = false;

    function doesSbeHandleLookup(config) {
        const compoundId = typeof config.makeId(0) === "object";
        const clustered =
            config.collOpts.hasOwnProperty("clusteredIndex") || allCollectionsClusteredByDefault;
        return !compoundId || clustered;
    }

    // Whether a post-move donor-side lookup reports the orphaned document as found.
    // TODO: SERVER-134080: non-clustered compound _ids move to SBE, which filters the orphan
    // correctly, so this stops being true.
    function reportsOrphanAsFound(config) {
        return !doesSbeHandleLookup(config) && Object.keys(config.key)[0] === "_id";
    }

    // Shards an already-populated collection, then guarantees the seed-0 and seed-1
    // documents end up in separate chunks, both on shard0, regardless of sharding
    // strategy. A single initial chunk (or, for hashed, a naive numInitialChunks pin)
    // would put both documents in the same chunk, so migrating "doc1's chunk" away later
    // would take doc0 with it. split: {find: ...} splits the chunk at that specific
    // document's own key (for hashed sharding, its actual hashed value, not its raw
    // value), so splitting at doc1's key deterministically separates it from doc0 either
    // way; the documents must already exist for 'find' to resolve.
    function shardAndPinToShard0(config, coll) {
        // shardCollection only auto-creates the supporting index for a genuinely empty
        // collection; this one is already populated, and a hashed key needs its own
        // index even on _id (the default _id_ index is ascending, not hashed).
        assert.commandWorked(coll.createIndex(config.key));
        assert.commandWorked(
            mongosDB.adminCommand({shardCollection: coll.getFullName(), key: config.key}),
        );
        assert.commandWorked(
            mongosDB.adminCommand({split: coll.getFullName(), find: shardKeyFilter(config, 1)}),
        );
        for (const seed of [0, 1]) {
            assert.commandWorked(
                mongosDB.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: shardKeyFilter(config, seed),
                    to: st.shard0.shardName,
                }),
            );
        }
    }

    before(function () {
        // These tests deliberately leave orphans behind (via suspended range deletion or
        // direct-to-shard inserts), so the orphan-checking hook must be disabled.
        TestData.skipCheckOrphans = true;

        st = new ShardingTest({
            shards: {rs0: {nodes: 1}, rs1: {nodes: 1}},
            mongos: 1,
        });
        mongosDB = st.s.getDB(jsTestName());
        // Whether the ambient clusterAllCollectionsByDefault failpoint is on (see
        // doesSbeHandleLookup): queried once here because it is fixed for the whole run.
        allCollectionsClusteredByDefault = ClusteredCollectionUtil.areAllCollectionsClustered(
            st.rs0.getPrimary(),
        );
        // $_internalSearchIdLookup always runs shard-local, so each shard's primary gets one
        // long-lived internal-client connection for the direct-to-shard commands below.
        internalDB0 = createInternalDB(st.rs0.getPrimary().host, mongosDB.getName());
        internalDB1 = createInternalDB(st.rs1.getPrimary().host, mongosDB.getName());
    });

    // Start each test from a pristine database whose primary is shard0.
    beforeEach(function () {
        assert.commandWorked(mongosDB.dropDatabase());
        assert.commandWorked(
            mongosDB.adminCommand({
                enableSharding: mongosDB.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );
    });

    after(function () {
        internalDB0.getMongo().close();
        internalDB1.getMongo().close();
        st.stop();
    });

    for (const config of shardKeyConfigs) {
        // A migrated chunk with range deletion suspended leaves a document physically
        // present, but no longer owned, on the donor: a real orphan, reproduced by the
        // migration itself rather than by computing shard placement by hand (which hashed
        // sharding would otherwise require, à la chunk_bounds_util.js).
        it(`running search with orphans left by a migrated chunk [${config.name}]`, function () {
            const coll = assertCreateCollection(
                mongosDB,
                "migrate_" + config.name,
                config.collOpts,
            );

            const stayingDoc = makeDoc(config, 0);
            const migratingDoc = makeDoc(config, 1);
            assert.writeOK(coll.insert([stayingDoc, migratingDoc]));

            shardAndPinToShard0(config, coll);

            withFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion", () => {
                assert.commandWorked(
                    mongosDB.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: shardKeyFilter(config, 1),
                        to: st.shard1.shardName,
                    }),
                );

                // The migration itself must have moved the data: shard0 keeps both physical
                // copies (deletion suspended), shard1 owns the migrated chunk.
                assertCollDataDistribution(mongosDB, coll, [
                    [st.shard0, 2],
                    [st.shard1, 1],
                ]);

                const shard0Version = ShardVersioningUtil.getShardVersion(
                    st.rs0.getPrimary(),
                    coll.getFullName(),
                    true /* waitForRefresh */,
                );
                const shard1Version = ShardVersioningUtil.getShardVersion(
                    st.rs1.getPrimary(),
                    coll.getFullName(),
                    true /* waitForRefresh */,
                );

                const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                    mongosDB,
                    () => {
                        // The mock-mongot fan-out feeds the stage the same {_id}-only documents
                        // $search would, and only needs any one physically-present document as its
                        // carrier (the orphan itself can be the donor's seed).
                        const donorResults = runAggWithMockMongotResults(
                            internalDB0,
                            coll.getName(),
                            [stayingDoc._id, migratingDoc._id],
                            {shardVersion: shard0Version},
                        );
                        assert.eq(
                            donorResults,
                            reportsOrphanAsFound(config)
                                ? [stayingDoc, migratingDoc]
                                : [stayingDoc],
                            {donorResults},
                        );

                        const recipientResults = runAggWithMockMongotResults(
                            internalDB1,
                            coll.getName(),
                            [migratingDoc._id],
                            {shardVersion: shard1Version},
                        );
                        assert.eq(recipientResults, [migratingDoc], {recipientResults});
                    },
                );

                // Donor: stayingDoc found, the orphan notFound (found for the known-issue cell,
                // reportsOrphanAsFound); recipient: migratingDoc found.
                const byEngine = readIdLookupDelta(delta);
                if (doesSbeHandleLookup(config)) {
                    assert.eq(
                        byEngine.sbe,
                        {found: 2, notFound: 1, notHandled: 0},
                        {
                            byEngine,
                            delta,
                        },
                    );
                    assert.eq(
                        byEngine.aggregation,
                        {found: 0, notFound: 0, notHandled: 0},
                        {
                            byEngine,
                            delta,
                        },
                    );
                } else {
                    assert.eq(
                        byEngine.sbe,
                        {found: 0, notFound: 0, notHandled: 3},
                        {
                            byEngine,
                            delta,
                        },
                    );
                    assert.eq(
                        byEngine.aggregation,
                        reportsOrphanAsFound(config)
                            ? {found: 3, notFound: 0, notHandled: 0}
                            : {found: 2, notFound: 1, notHandled: 0},
                        {
                            byEngine,
                            delta,
                        },
                    );
                }
            });
        });

        // A query opened before a moveChunk and resumed after it. batchSize: 0 parks the cursor
        // and its collection acquisition before $_internalSearchIdLookup has run over any
        // document, the moveChunk lands while it sits idle, and the getMore resumes it through
        // the pre-move acquisition: the shard version is pinned for the life of the query, so
        // the resumed read reports pre-migrated results, while any newly issued query sees the
        // post-move state.
        it(`migrating chunks while a search pipeline is running reports pre-migrated results [${config.name}]`, function () {
            const collName = "parked_" + config.name;
            const coll = assertCreateCollection(mongosDB, collName, config.collOpts);

            const stayingDoc = makeDoc(config, 0);
            const migratingDoc = makeDoc(config, 1);
            assert.writeOK(coll.insert([stayingDoc, migratingDoc]));

            shardAndPinToShard0(config, coll);

            const shard0VersionAtOpen = ShardVersioningUtil.getShardVersion(
                st.rs0.getPrimary(),
                coll.getFullName(),
                true,
            );
            const lookupIds = [stayingDoc._id, migratingDoc._id];

            // Suspending range deletion keeps 'migratingDoc' physically present on shard0, so
            // the fresh-version re-query below must drop it as an unowned orphan rather than
            // merely fail to find a deleted document.
            withFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion", () => {
                // runAggWithMockMongot's default cursor {batchSize: 0} defers idLookup's work
                // to the first getMore, parking the cursor before any DDL happens.
                const response = runAggWithMockMongot(internalDB0, collName, lookupIds, {
                    shardVersion: shard0VersionAtOpen,
                });
                assert.eq(response.cursor.firstBatch, [], {response});

                assert.commandWorked(
                    mongosDB.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: shardKeyFilter(config, 1),
                        to: st.shard1.shardName,
                    }),
                );
                assertCollDataDistribution(mongosDB, coll, [
                    [st.shard0, 2],
                    [st.shard1, 1],
                ]);

                // A brand-new query cannot read through the pinned version: the pre-move
                // shardVersion is rejected with StaleConfig, forcing the client to re-route.
                assert.commandFailedWithCode(
                    internalDB0.runCommand({
                        aggregate: collName,
                        pipeline: mockMongotPipeline(lookupIds),
                        cursor: {},
                        shardVersion: shard0VersionAtOpen,
                        readConcern: {},
                        writeConcern: {},
                    }),
                    ErrorCodes.StaleConfig,
                );

                // The shard version is pinned for the life of the query, even under
                // readConcern local, so the resumed getMore serves the pre-move state:
                // 'migratingDoc' is returned even though its chunk now belongs to shard1.
                const getMoreDelta = ServerStatusMetrics.withServerStatusMetrics(
                    internalDB0,
                    () => {
                        const getMoreResponse = assert.commandWorked(
                            internalDB0.runCommand({
                                getMore: response.cursor.id,
                                collection: collName,
                            }),
                        );
                        assert.eq(getMoreResponse.cursor.nextBatch, [stayingDoc, migratingDoc], {
                            getMoreResponse,
                        });
                    },
                );
                const getMoreByEngine = readIdLookupDelta(getMoreDelta);
                if (doesSbeHandleLookup(config)) {
                    assert.eq(
                        getMoreByEngine.sbe,
                        {found: 2, notFound: 0, notHandled: 0},
                        {
                            getMoreByEngine,
                            getMoreDelta,
                        },
                    );
                    assert.eq(
                        getMoreByEngine.aggregation,
                        {found: 0, notFound: 0, notHandled: 0},
                        {getMoreByEngine, getMoreDelta},
                    );
                } else {
                    assert.eq(
                        getMoreByEngine.sbe,
                        {found: 0, notFound: 0, notHandled: 2},
                        {
                            getMoreByEngine,
                            getMoreDelta,
                        },
                    );
                    assert.eq(
                        getMoreByEngine.aggregation,
                        {found: 2, notFound: 0, notHandled: 0},
                        {getMoreByEngine, getMoreDelta},
                    );
                }

                // Re-issued with a fresh shardVersion, the query sees the post-move state:
                // migratingDoc is still physically on shard0, so it must be dropped by the
                // shard filter as an unowned orphan (or reported as found for the known-issue
                // cell, reportsOrphanAsFound).
                const freshDelta = ServerStatusMetrics.withServerStatusMetrics(internalDB0, () => {
                    const freshResults = runAggWithMockMongotResults(
                        internalDB0,
                        collName,
                        lookupIds,
                        {
                            shardVersion: ShardVersioningUtil.getShardVersion(
                                st.rs0.getPrimary(),
                                coll.getFullName(),
                                true,
                            ),
                        },
                    );
                    assert.eq(
                        freshResults,
                        reportsOrphanAsFound(config) ? [stayingDoc, migratingDoc] : [stayingDoc],
                        {freshResults},
                    );
                });
                const freshByEngine = readIdLookupDelta(freshDelta);
                if (doesSbeHandleLookup(config)) {
                    assert.eq(
                        freshByEngine.sbe,
                        {found: 1, notFound: 1, notHandled: 0},
                        {
                            freshByEngine,
                            freshDelta,
                        },
                    );
                    assert.eq(
                        freshByEngine.aggregation,
                        {found: 0, notFound: 0, notHandled: 0},
                        {freshByEngine, freshDelta},
                    );
                } else {
                    assert.eq(
                        freshByEngine.sbe,
                        {found: 0, notFound: 0, notHandled: 2},
                        {
                            freshByEngine,
                            freshDelta,
                        },
                    );
                    assert.eq(
                        freshByEngine.aggregation,
                        reportsOrphanAsFound(config)
                            ? {found: 2, notFound: 0, notHandled: 0}
                            : {found: 1, notFound: 1, notHandled: 0},
                        {freshByEngine, freshDelta},
                    );
                }
            });
        });

        describe("reshardCollection", function () {
            function reshardSplitAtDoc1(coll) {
                return mongosDB.adminCommand({
                    reshardCollection: coll.getFullName(),
                    key: {rk: 1},

                    // The split mirrors the moveChunk tests' observable placement (doc0's range
                    // stays on shard0, doc1's moves to shard1) while the shard-key shape itself
                    // changes to {rk: 1}: the one interaction moveChunk cannot exercise.
                    shardDistribution: [
                        {shard: st.shard0.shardName, min: {rk: MinKey}, max: {rk: 1}},
                        {shard: st.shard1.shardName, min: {rk: 1}, max: {rk: MaxKey}},
                    ],
                });
            }

            function setupReshardableCollection(collName) {
                const coll = assertCreateCollection(mongosDB, collName, config.collOpts);

                // 'rk' is the reshard target key's field, seeded to mirror the existing shard-key
                // values so the reshard's split at rk=1 separates the two documents.
                const stayingDoc = {...makeDoc(config, 0), rk: 0};
                const migratingDoc = {...makeDoc(config, 1), rk: 1};
                assert.writeOK(coll.insert([stayingDoc, migratingDoc]));
                shardAndPinToShard0(config, coll);
                return {coll, stayingDoc, migratingDoc};
            }

            // A query opened before a reshardCollection and resumed after it. batchSize: 0 parks
            // the cursor and its collection acquisition before $_internalSearchIdLookup has run
            // over any document, the reshard's rename swap lands while it sits idle.
            it(`reshards while a search pipeline is parked between getMores [${config.name}]`, function () {
                const collName = "reshard_parked_" + config.name;
                const {coll, stayingDoc, migratingDoc} = setupReshardableCollection(collName);

                const shard0VersionAtOpen = ShardVersioningUtil.getShardVersion(
                    st.rs0.getPrimary(),
                    coll.getFullName(),
                    true,
                );
                const lookupIds = [stayingDoc._id, migratingDoc._id];

                // runAggWithMockMongot's default cursor {batchSize: 0} defers idLookup's work to
                // the first getMore, parking the cursor before any DDL happens.
                const response = runAggWithMockMongot(internalDB0, collName, lookupIds, {
                    shardVersion: shard0VersionAtOpen,
                });
                assert.eq(response.cursor.firstBatch, [], {response});

                assert.commandWorked(reshardSplitAtDoc1(coll));

                // A completed reshard leaves no orphans: shard0's post-reshard collection is the
                // cloned temporary one containing only the range it owns.
                assertCollDataDistribution(mongosDB, coll, [
                    [st.shard0, 1],
                    [st.shard1, 1],
                ]);

                // A brand-new query cannot read through the pinned version: the pre-reshard
                // shardVersion is rejected with StaleConfig, forcing the client to re-route.
                assert.commandFailedWithCode(
                    internalDB0.runCommand({
                        aggregate: collName,
                        pipeline: mockMongotPipeline(lookupIds),
                        cursor: {},
                        shardVersion: shard0VersionAtOpen,
                        readConcern: {},
                        writeConcern: {},
                    }),
                    ErrorCodes.StaleConfig,
                );

                // On getMore the collection acquisition fails on UUID mismatch, failing the query.
                const getMoreDelta = ServerStatusMetrics.withServerStatusMetrics(
                    internalDB0,
                    () => {
                        assert.commandFailedWithCode(
                            internalDB0.runCommand({
                                getMore: response.cursor.id,
                                collection: collName,
                            }),
                            ErrorCodes.QueryPlanKilled,
                        );
                    },
                );
                const getMoreByEngine = readIdLookupDelta(getMoreDelta);
                assert.eq(
                    getMoreByEngine.sbe,
                    {found: 0, notFound: 0, notHandled: 0},
                    {getMoreByEngine, getMoreDelta},
                );
                assert.eq(
                    getMoreByEngine.aggregation,
                    {found: 0, notFound: 0, notHandled: 0},
                    {getMoreByEngine, getMoreDelta},
                );

                // Re-issued with a fresh shardVersion, the query sees the post-reshard state:
                // migratingDoc is absent from shard0's new collection.
                const serverStatusDelta = ServerStatusMetrics.withServerStatusMetrics(
                    internalDB0,
                    () => {
                        const freshResults = runAggWithMockMongotResults(
                            internalDB0,
                            collName,
                            lookupIds,
                            {
                                shardVersion: ShardVersioningUtil.getShardVersion(
                                    st.rs0.getPrimary(),
                                    coll.getFullName(),
                                    true,
                                ),
                            },
                        );
                        assert.eq(freshResults, [stayingDoc], {freshResults});
                    },
                );
                const engineStats = readIdLookupDelta(serverStatusDelta);
                if (doesSbeHandleLookup(config)) {
                    assert.eq(
                        engineStats.sbe,
                        {found: 1, notFound: 1, notHandled: 0},
                        {engineStats, serverStatusDelta},
                    );
                    assert.eq(
                        engineStats.aggregation,
                        {found: 0, notFound: 0, notHandled: 0},
                        {engineStats, serverStatusDelta},
                    );
                } else {
                    assert.eq(
                        engineStats.sbe,
                        {found: 0, notFound: 0, notHandled: 2},
                        {engineStats, serverStatusDelta},
                    );
                    assert.eq(
                        engineStats.aggregation,
                        {found: 1, notFound: 1, notHandled: 0},
                        {engineStats, serverStatusDelta},
                    );
                }
            });

            // The completed-before case. Unlike a moveChunk with suspended range deletion, there
            // is no orphan to filter: the donor's lookup misses the moved document as a clean
            // notFound, and the recipient finds it on its new owner.
            it(`reshards before the search query runs, leaving no orphans [${config.name}]`, function () {
                const collName = "reshard_before_" + config.name;
                const {coll, stayingDoc, migratingDoc} = setupReshardableCollection(collName);

                assert.commandWorked(reshardSplitAtDoc1(coll));
                assertCollDataDistribution(mongosDB, coll, [
                    [st.shard0, 1],
                    [st.shard1, 1],
                ]);

                const shard0Version = ShardVersioningUtil.getShardVersion(
                    st.rs0.getPrimary(),
                    coll.getFullName(),
                    true,
                );
                const shard1Version = ShardVersioningUtil.getShardVersion(
                    st.rs1.getPrimary(),
                    coll.getFullName(),
                    true,
                );

                const serverStatusDelta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                    mongosDB,
                    () => {
                        const donorResults = runAggWithMockMongotResults(
                            internalDB0,
                            coll.getName(),
                            [stayingDoc._id, migratingDoc._id],
                            {shardVersion: shard0Version},
                        );
                        assert.eq(donorResults, [stayingDoc], {donorResults});

                        const recipientResults = runAggWithMockMongotResults(
                            internalDB1,
                            coll.getName(),
                            [migratingDoc._id],
                            {shardVersion: shard1Version},
                        );
                        assert.eq(recipientResults, [migratingDoc], {recipientResults});
                    },
                );

                // Donor: stayingDoc found, migratingDoc notFound (absent, not an orphan);
                // recipient: migratingDoc found. Same observable counts as the moveChunk live
                // test, but through absence rather than orphan filtering.
                const engineStats = readIdLookupDelta(serverStatusDelta);
                if (doesSbeHandleLookup(config)) {
                    assert.eq(
                        engineStats.sbe,
                        {found: 2, notFound: 1, notHandled: 0},
                        {engineStats, serverStatusDelta},
                    );
                    assert.eq(
                        engineStats.aggregation,
                        {found: 0, notFound: 0, notHandled: 0},
                        {engineStats, serverStatusDelta},
                    );
                } else {
                    assert.eq(
                        engineStats.sbe,
                        {found: 0, notFound: 0, notHandled: 3},
                        {engineStats, serverStatusDelta},
                    );
                    assert.eq(
                        engineStats.aggregation,
                        {found: 2, notFound: 1, notHandled: 0},
                        {engineStats, serverStatusDelta},
                    );
                }
            });
        });
    }

    // A search index defined on a view runs mongot's own lookup against the *underlying*
    // sharded collection, then $search's desugaring sets 'viewPipeline' on the spec so
    // $_internalSearchIdLookup reapplies the view's defining pipeline after the raw
    // document is resolved.
    describe("view-backed lookups", function () {
        it("does not filter the orphan out on a _id-sharded collection", function () {
            const collName = "viewbacked";
            const coll = assertCreateCollection(mongosDB, collName);
            assert.commandWorked(
                mongosDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
            );
            assert.commandWorked(
                mongosDB.adminCommand({split: coll.getFullName(), middle: {_id: 0}}),
            );

            // Both chunks start on shard0 (the database primary): ownedDoc stays in [MinKey, 0),
            // orphanDoc lives in [0, MaxKey), which moves off to shard1 below.
            const ownedDoc = {_id: -1, x: "owned"};
            const orphanDoc = {_id: 1, x: "orphan"};
            assert.writeOK(coll.insert([ownedDoc, orphanDoc]));

            withFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion", () => {
                assert.commandWorked(
                    mongosDB.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: {_id: orphanDoc._id},
                        to: st.shard1.shardName,
                    }),
                );
                assertCollDataDistribution(mongosDB, coll, [
                    [st.shard0, 2],
                    [st.shard1, 1],
                ]);

                const shard0Version = ShardVersioningUtil.getShardVersion(
                    st.rs0.getPrimary(),
                    coll.getFullName(),
                    true,
                );
                const delta = ServerStatusMetrics.withServerStatusMetrics(internalDB0, () => {
                    const results = runAggWithMockMongotResults(
                        internalDB0,
                        collName,
                        [ownedDoc._id, orphanDoc._id],
                        {
                            idLookupSpec: {viewPipeline: [{$addFields: {viewTag: "viewed"}}]},
                            shardVersion: shard0Version,
                        },
                    );
                    assert.eq(
                        results,
                        [ownedDoc, orphanDoc].map((doc) => ({...doc, viewTag: "viewed"})),
                        {results},
                    );
                });

                // TODO SERVER-134686: a viewPipeline forces the aggregation executor, which
                // never requests shard filtering, so the orphan is reported as found. Flip to
                // expecting it to be filtered as an orphan once fixed.
                const byEngine = readIdLookupDelta(delta);
                assert.eq(byEngine.sbe, {found: 0, notFound: 0, notHandled: 0}, {byEngine, delta});
                assert.eq(
                    byEngine.aggregation,
                    {found: 2, notFound: 0, notHandled: 0},
                    {byEngine, delta},
                );
            });
        });

        it("filters the orphan out on a non-_id-sharded collection", function () {
            const config = {key: {sk: 1}, makeId: (seed) => seed, collOpts: {}};
            const coll = assertCreateCollection(mongosDB, "viewbacked_sk", config.collOpts);

            const stayingDoc = makeDoc(config, 0);
            const migratingDoc = makeDoc(config, 1);
            assert.writeOK(coll.insert([stayingDoc, migratingDoc]));

            shardAndPinToShard0(config, coll);

            withFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion", () => {
                assert.commandWorked(
                    mongosDB.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: shardKeyFilter(config, 1),
                        to: st.shard1.shardName,
                    }),
                );
                assertCollDataDistribution(mongosDB, coll, [
                    [st.shard0, 2],
                    [st.shard1, 1],
                ]);

                const shard0Version = ShardVersioningUtil.getShardVersion(
                    st.rs0.getPrimary(),
                    coll.getFullName(),
                    true /* waitForRefresh */,
                );

                const delta = ServerStatusMetrics.withServerStatusMetrics(internalDB0, () => {
                    const results = runAggWithMockMongotResults(
                        internalDB0,
                        coll.getName(),
                        [stayingDoc._id, migratingDoc._id],
                        {
                            idLookupSpec: {viewPipeline: [{$addFields: {viewTag: "viewed"}}]},
                            shardVersion: shard0Version,
                        },
                    );

                    // The orphan is filtered: only the owned document comes back, with the
                    // view's defining pipeline reapplied.
                    assert.eq(results, [{...stayingDoc, viewTag: "viewed"}], {results});
                });

                // The view pipeline forces the aggregation engine: the owned doc found, the orphan
                // filtered and reported notFound.
                const byEngine = readIdLookupDelta(delta);
                assert.eq(byEngine.sbe, {found: 0, notFound: 0, notHandled: 0}, {byEngine, delta});
                assert.eq(
                    byEngine.aggregation,
                    {found: 1, notFound: 1, notHandled: 0},
                    {byEngine, delta},
                );
            });
        });
    });
});
