/**
 * A sharded $search that requires $$SEARCH_META promises metadata cursors to the merging
 * pipeline (planShardedSearch returns a metadata merge protocol version). If a shard's mongot
 * then answers with a single unlabeled cursor (the response shape of a mongot that does not
 * support metadata cursors), no metadata cursor is returned to the merging pipeline, leaving the
 * merge half with a $setVariableFromSubPipeline stage that has no cursor source. The router
 * asserts that this cannot happen (an internal invariant) and fails the operation.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {mongotCommandForQuery} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {ShardingTestWithMongotMock} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = jsTestName();
const collName = "search";
const searchQuery = {query: "cakes", path: "title"};
const protocolVersion = NumberInt(42);

describe("sharded $search whose shard mongot returns an unlabeled cursor", function () {
    let stWithMock;
    let st;
    let coll;

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
        coll = testDB.getCollection(collName);
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, title: "cakes"},
                {_id: 2, title: "cookies and cakes"},
                {_id: 3, title: "vegetables"},
                {_id: 4, title: "take and bake cakes"},
            ]),
        );
        st.shardColl(coll, {_id: 1}, {_id: 3}, {_id: 3}, dbName);
    });

    after(function () {
        // The router's invariant assertion (tassert 13182000) triggered above makes the mongos
        // exit with a non-zero code at shutdown in testing environments, so skip exit-code
        // validation.
        stWithMock.stop({skipValidatingExitCode: true});
    });

    it("asserts the metadata-cursor invariant instead of dropping $$SEARCH_META", function () {
        // mongos' planShardedSearch promises a metadata merging pipeline.
        const mongosMongot = stWithMock.getMockConnectedToHost(st.s);
        mongosMongot.setMockResponses(
            [
                {
                    expectedCommand: {
                        planShardedSearch: collName,
                        query: searchQuery,
                        $db: dbName,
                        searchFeatures: {shardedSort: 1},
                    },
                    response: {
                        ok: 1,
                        protocolVersion: protocolVersion,
                        metaPipeline: [{$group: {_id: {type: "$type"}, value: {$sum: "$metaVal"}}}],
                    },
                },
            ],
            1,
        );

        // Each shard's mongot answers the dispatched search with a single UNLABELED cursor (no
        // cursor type, no vars) — the response shape of a mongot that does not support metadata
        // cursors. Arm both the sharded (with 'intermediate') and unsharded command forms.
        let mockCursorId = 100;
        const armShard = (rs, docs) => {
            const shardPrimary = rs.getPrimary();
            const collUUID = getUUIDFromListCollections(shardPrimary.getDB(dbName), collName);
            const mongot = stWithMock.getMockConnectedToHost(shardPrimary);
            for (const pv of [protocolVersion, null]) {
                mongot.setMockResponses(
                    [
                        {
                            expectedCommand: mongotCommandForQuery({
                                query: searchQuery,
                                collName: collName,
                                db: dbName,
                                collectionUUID: collUUID,
                                protocolVersion: pv,
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
            {_id: 1, $searchScore: 0.99},
            {_id: 2, $searchScore: 0.5},
        ]);
        armShard(st.rs1, [
            {_id: 3, $searchScore: 0.33},
            {_id: 4, $searchScore: 0.2},
        ]);

        const res = st.s.getDB(dbName).runCommand({
            aggregate: collName,
            pipeline: [{$search: searchQuery}, {$project: {meta: "$$SEARCH_META"}}],
            cursor: {},
        });
        jsTest.log.info("Aggregate result", {res});
        assert.commandFailedWithCode(res, 13182000);
    });
});
