/**
 * Tests that `$_internalSearchIdLookup` filters orphaned documents (left behind on a shard by a
 * chunk migration) when its lookup runs through the aggregation-fallback local-read executor,
 * across the view-backed lookup scenarios that land on that executor.
 *
 * @tags: [requires_fcv_90]
 */
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertCollDataDistribution} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

// $_internalSearchIdLookup is internal-only: rejected in user requests and accepted only from an
// internal client, which must specify an explicit (empty) read/write concern on every command
// that accepts one.
function createInternalDB(hostOrUri, dbName) {
    const conn = new Mongo(hostOrUri);
    assert.commandWorked(
        conn.getDB("admin").runCommand({
            hello: 1,
            internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
        }),
    );
    return conn.getDB(dbName);
}

function runIdLookupAggregate(internalDB, collectionName, pipeline, extra = {}) {
    return assert.commandWorked(
        internalDB.runCommand({
            aggregate: collectionName,
            pipeline: pipeline,
            cursor: {},
            readConcern: {},
            writeConcern: {},
            ...extra,
        }),
    );
}

// Fans a real document out into one document per entry of 'mockIds', as if $search had handed
// $_internalSearchIdLookup 'mockIds' as its own index results. $_internalSearchIdLookup always
// looks up within the aggregate command's own namespace, so this seeds off one real document
// ($limit: 1) rather than $documents.
function mockMongotPipeline(mockIds, idLookupSpec = {}) {
    return [
        {$limit: 1},
        {$addFields: {__mongot: mockIds.map((_id) => ({_id}))}},
        {$unwind: "$__mongot"},
        {$replaceWith: "$__mongot"},
        {$_internalSearchIdLookup: idLookupSpec},
    ];
}

function runAggWithMockMongotResults(
    internalDB,
    collectionName,
    mockIds,
    {idLookupSpec = {}, ...extra} = {},
) {
    const res = runIdLookupAggregate(
        internalDB,
        collectionName,
        mockMongotPipeline(mockIds, idLookupSpec),
        extra,
    );
    return new DBCommandCursor(internalDB, res).toArray();
}

describe("$_internalSearchIdLookup orphan filtering via the aggregation fallback", function () {
    let st;
    let mongosDB;
    let internalDB0;

    before(function () {
        // These tests deliberately leave orphans behind (via suspended range deletion), so the
        // orphan-checking hook must be disabled.
        TestData.skipCheckOrphans = true;

        st = new ShardingTest({
            shards: {rs0: {nodes: 1}, rs1: {nodes: 1}},
            mongos: 1,
        });
        mongosDB = st.s.getDB(jsTestName());
        // $_internalSearchIdLookup always runs shard-local, so shard0's primary gets one
        // long-lived internal-client connection for the direct-to-shard commands below.
        internalDB0 = createInternalDB(st.rs0.getPrimary().host, mongosDB.getName());
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
        st.stop();
    });

    // $search's desugaring sets 'viewPipeline' on the idLookup spec so
    // $_internalSearchIdLookup reapplies the view's defining pipeline after the raw document is
    // resolved. A view pipeline forces the aggregation executor, which must run its shard-local
    // lookup with shard filtering.
    it("filters the orphan out of a view-backed lookup on a _id-sharded collection", function () {
        const collName = "viewbacked";
        const coll = assertCreateCollection(mongosDB, collName);
        assert.commandWorked(
            mongosDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
        );
        assert.commandWorked(mongosDB.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));

        // Both chunks start on shard0 (the database primary): ownedDoc stays in [MinKey, 0),
        // orphanDoc lives in [0, MaxKey), which moves off to shard1 below.
        const ownedDoc = {_id: -1, x: "owned"};
        const orphanDoc = {_id: 1, x: "orphan"};
        assert.writeOK(coll.insert([ownedDoc, orphanDoc]));

        const failPoint = configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");
        try {
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
                true /* waitForRefresh */,
            );
            const results = runAggWithMockMongotResults(
                internalDB0,
                collName,
                [ownedDoc._id, orphanDoc._id],
                {
                    idLookupSpec: {viewPipeline: [{$addFields: {viewTag: "viewed"}}]},
                    shardVersion: shard0Version,
                },
            );

            // The orphan is filtered: only the owned document comes back, with the view's
            // defining pipeline reapplied.
            assert.eq(results, [{...ownedDoc, viewTag: "viewed"}], {results});
        } finally {
            failPoint.off();
        }
    });

    it("filters the orphan out of a view-backed lookup on a non-_id-sharded collection", function () {
        const coll = assertCreateCollection(mongosDB, "viewbacked_sk");

        const stayingDoc = {_id: 0, sk: 0};
        const migratingDoc = {_id: 1, sk: 1};
        assert.writeOK(coll.insert([stayingDoc, migratingDoc]));

        // Guarantee both documents end up in separate chunks, both on shard0: split at doc1's own
        // key, then pin each chunk to shard0 so migrating "doc1's chunk" away later takes only
        // doc1 with it.
        assert.commandWorked(coll.createIndex({sk: 1}));
        assert.commandWorked(
            mongosDB.adminCommand({shardCollection: coll.getFullName(), key: {sk: 1}}),
        );
        assert.commandWorked(mongosDB.adminCommand({split: coll.getFullName(), find: {sk: 1}}));
        for (const sk of [stayingDoc.sk, migratingDoc.sk]) {
            assert.commandWorked(
                mongosDB.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {sk: sk},
                    to: st.shard0.shardName,
                }),
            );
        }

        const failPoint = configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");
        try {
            assert.commandWorked(
                mongosDB.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {sk: migratingDoc.sk},
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
            const results = runAggWithMockMongotResults(
                internalDB0,
                coll.getName(),
                [stayingDoc._id, migratingDoc._id],
                {
                    idLookupSpec: {viewPipeline: [{$addFields: {viewTag: "viewed"}}]},
                    shardVersion: shard0Version,
                },
            );

            // The orphan is filtered: only the owned document comes back, with the view's
            // defining pipeline reapplied.
            assert.eq(results, [{...stayingDoc, viewTag: "viewed"}], {results});
        } finally {
            failPoint.off();
        }
    });

    // A $sort+$group view pipeline qualifies for the DISTINCT_SCAN rewrite; with an index on the
    // grouped field, the idLookup sub-pipeline can be planned as a distinct executor. That path
    // must not skip the forced shard filter by resetting to default planner options.
    it("filters the orphan out through a DISTINCT_SCAN-eligible view pipeline on a _id-sharded collection", function () {
        const collName = "viewbacked_distinct";
        const coll = assertCreateCollection(mongosDB, collName);
        assert.commandWorked(
            mongosDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
        );
        assert.commandWorked(mongosDB.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
        // Index backing the view pipeline's $sort/$group so DISTINCT_SCAN is plannable.
        assert.commandWorked(coll.createIndex({x: 1}));

        const ownedDoc = {_id: -1, x: -1, y: "owned"};
        const orphanDoc = {_id: 1, x: 1, y: "orphan"};
        assert.writeOK(coll.insert([ownedDoc, orphanDoc]));

        const failPoint = configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");
        try {
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
                true /* waitForRefresh */,
            );
            const results = runAggWithMockMongotResults(
                internalDB0,
                collName,
                [ownedDoc._id, orphanDoc._id],
                {
                    idLookupSpec: {
                        viewPipeline: [
                            {$sort: {x: 1}},
                            {$group: {_id: "$x", firstY: {$first: "$y"}}},
                        ],
                    },
                    shardVersion: shard0Version,
                },
            );

            // The orphan is filtered before the view pipeline runs, so only ownedDoc's group
            // survives the (DISTINCT_SCAN-eligible) view transform.
            assert.eq(results, [{_id: -1, firstY: "owned"}], {results});
        } finally {
            failPoint.off();
        }
    });
});
