/**
 * A sharded $search that does not reference $$SEARCH_META must not attach a
 * $setVariableFromSubPipeline stage at split time, even when pipeline optimization is skipped
 * (previously the unreferenced stage survived an unoptimized split, and with unlabeled shard
 * cursors the unpinned merge half was delegated to a shard, which tripped a tripwire assertion).
 * The query must succeed.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {mongotCommandForQuery} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {ShardingTestWithMongotMock} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = jsTestName();
const searchQuery = {query: "cakes", path: "title"};
const protocolVersion = NumberInt(42);

describe("unoptimized sharded $search whose shard mongot returns unlabeled cursors", function () {
    let stWithMock;
    let st;
    let coll;
    let failpointConns;

    before(function () {
        stWithMock = new ShardingTestWithMongotMock({
            name: jsTestName(),
            shards: {rs0: {nodes: 1}, rs1: {nodes: 1}},
            mongos: 1,
        });
        stWithMock.start();
        st = stWithMock.st;

        const testDB = st.s.getDB(dbName);
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}),
        );
        coll = testDB.getCollection("issues");
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, title: "cakes", assetid: 10},
                {_id: 2, title: "cookies and cakes", assetid: 11},
                {_id: 3, title: "vegetables", assetid: 10},
                {_id: 4, title: "take and bake cakes", assetid: 12},
            ]),
        );
        assert.commandWorked(
            testDB.assets.insertMany([
                {assetid: 10, name: "a10"},
                {assetid: 11, name: "a11"},
            ]),
        );
        st.shardColl(coll, {_id: 1}, {_id: 3}, {_id: 3}, dbName);

        // Skipping optimization is what leaves the unreferenced $setVariableFromSubPipeline in
        // the merge half (in production this happens when e.g. the routing table is unavailable).
        failpointConns = [st.s, st.rs0.getPrimary(), st.rs1.getPrimary()];
        for (const conn of failpointConns) {
            assert.commandWorked(
                conn.adminCommand({
                    configureFailPoint: "disablePipelineOptimization",
                    mode: "alwaysOn",
                }),
            );
        }
    });

    after(function () {
        for (const conn of failpointConns) {
            assert.commandWorked(
                conn.adminCommand({configureFailPoint: "disablePipelineOptimization", mode: "off"}),
            );
        }
        stWithMock.stop();
    });

    it("succeeds without attaching an unread SEARCH_META merging stage", function () {
        const mongosMongot = stWithMock.getMockConnectedToHost(st.s);
        mongosMongot.setMockResponses(
            [
                {
                    expectedCommand: {
                        planShardedSearch: coll.getName(),
                        query: searchQuery,
                        $db: dbName,
                        searchFeatures: {shardedSort: 1},
                    },
                    response: {
                        ok: 1,
                        protocolVersion: protocolVersion,
                        metaPipeline: [{$group: {_id: null, count: {$sum: "$count"}}}],
                    },
                    maybeUnused: true,
                },
            ],
            1,
        );

        // Shards' mongot answers single UNLABELED cursors, so no metadata cursors reach the
        // router and meta-cursor injection is skipped.
        let mockCursorId = 100;
        const armShard = (rs, docs) => {
            const shardPrimary = rs.getPrimary();
            const collUUID = getUUIDFromListCollections(shardPrimary.getDB(dbName), coll.getName());
            const mongot = stWithMock.getMockConnectedToHost(shardPrimary);
            for (let i = 0; i < 3; i++) {
                mongot.setMockResponses(
                    [
                        {
                            expectedCommand: mongotCommandForQuery({
                                query: searchQuery,
                                collName: coll.getName(),
                                db: dbName,
                                collectionUUID: collUUID,
                                protocolVersion: protocolVersion,
                            }),
                            response: {
                                ok: 1,
                                cursor: {
                                    id: NumberLong(0),
                                    ns: coll.getFullName(),
                                    nextBatch: docs,
                                },
                            },
                            maybeUnused: true,
                        },
                    ],
                    mockCursorId++,
                );
            }
        };
        armShard(st.rs0, [
            {_id: 1, $searchScore: 0.99, assetid: 10},
            {_id: 2, $searchScore: 0.5, assetid: 11},
        ]);
        armShard(st.rs1, [{_id: 4, $searchScore: 0.33, assetid: 12}]);

        // No $$SEARCH_META reference anywhere; the $lookup from the unsharded collection is what
        // draws the unpinned merge half to a shard.
        const res = st.s.getDB(dbName).runCommand({
            aggregate: coll.getName(),
            pipeline: [
                {$search: searchQuery},
                {
                    $lookup: {
                        from: "assets",
                        as: "asset",
                        localField: "assetid",
                        foreignField: "assetid",
                        pipeline: [{$project: {name: 1, _id: 0}}],
                    },
                },
                {$unwind: {path: "$asset", preserveNullAndEmptyArrays: true}},
                {$addFields: {assetname: "$asset.name"}},
                {$sort: {title: -1}},
                {$limit: 25},
            ],
            cursor: {batchSize: 2},
            allowDiskUse: true,
        });
        jsTest.log.info("Aggregate result", {res});
        assert.commandWorked(res);
        const docs = new DBCommandCursor(st.s.getDB(dbName), res).toArray();
        assert.eq(docs.length, 3, "expected all mocked search results", {docs});
        for (const doc of docs) {
            assert(!doc.hasOwnProperty("meta"), "unexpected SEARCH_META in result", {doc});
        }
    });
});
