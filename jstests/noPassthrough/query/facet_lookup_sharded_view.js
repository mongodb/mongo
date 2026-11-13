/**
 * Tests that a view named by a stage nested inside a $facet sub-pipeline resolves through mongos:
 * the view's pipeline must be applied, so the results must equal the view's own documents rather
 * than the backing collection's. Covers a $lookup inside another $lookup's sub-pipeline and a
 * $unionWith inside a $lookup's sub-pipeline.
 *
 * @tags: [
 *    requires_sharding,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("a view named by a nested $lookup inside $facet resolves through mongos", function () {
    let sharded;
    let testDB;

    before(function () {
        sharded = new ShardingTest({
            mongos: 1,
            shards: 1,
            config: 1,
        });

        const testDBName = "test";
        assert.commandWorked(
            sharded.s.adminCommand({
                enableSharding: testDBName,
                primaryShard: sharded.shard0.shardName,
            }),
        );
        testDB = sharded.getDB(testDBName);

        // The collection the aggregate runs on. It is sharded so the query goes through the routed
        // aggregation machinery where $facet sub-pipelines are dispatched from the router.
        const source = testDB.source;
        assert.commandWorked(source.insertOne({_id: 0}));
        assert.commandWorked(
            sharded.s.adminCommand({shardCollection: source.getFullName(), key: {_id: 1}}),
        );

        // The outer $lookup's target, an ordinary collection.
        const joinInput = testDB.joinInput;
        assert.commandWorked(joinInput.insertOne({_id: 0}));

        // The backing collection the view hides documents in. The 'private' documents and their
        // secrets must never surface in results read through the view.
        const backing = testDB.backing;
        assert.commandWorked(
            backing.insertMany([
                {_id: 0, tenant: "public", secret: "public-secret"},
                {_id: 1, tenant: "private", secret: "hidden-secret-1"},
                {_id: 2, tenant: "private", secret: "hidden-secret-2"},
            ]),
        );

        // The view filters out the private documents and marks its output so we can tell the
        // view's pipeline was applied.
        assert.commandWorked(
            testDB.createView("view", backing.getName(), [
                {$match: {tenant: "public"}},
                {$addFields: {fromView: true}},
            ]),
        );
    });

    after(function () {
        sharded.stop();
    });

    it("applies the view's pipeline, returning the view's documents, not the backing's", function () {
        const viewDocs = testDB.view.find().toArray();
        assert.eq(viewDocs.length, 1, {viewDocs});
        assert.eq(viewDocs[0].fromView, true, {viewDocs});

        // The $lookup inside a $lookup inside $facet: only the view's documents
        // should come back, not the backing collection's documents.
        const result = testDB.source
            .aggregate([
                {
                    $facet: {
                        branch: [
                            {
                                $lookup: {
                                    from: testDB.joinInput.getName(),
                                    pipeline: [
                                        {
                                            $lookup: {
                                                from: "view",
                                                pipeline: [],
                                                as: "inner",
                                            },
                                        },
                                    ],
                                    as: "outer",
                                },
                            },
                        ],
                    },
                },
            ])
            .toArray();

        assert.eq(result.length, 1, {result});
        const branchDocs = result[0].branch;
        assert.eq(branchDocs.length, 1, {result});

        const outerDocs = branchDocs[0].outer;
        assert.eq(outerDocs.length, 1, {branchDocs});
        assert.eq(outerDocs[0]._id, 0, {outerDocs});

        assertArrayEq({actual: outerDocs[0].inner, expected: viewDocs});
    });

    it("applies the view's pipeline for a $unionWith nested in a $lookup sub-pipeline", function () {
        const viewDocs = testDB.view.find().toArray();
        assert.eq(viewDocs.length, 1, {viewDocs});
        assert.eq(viewDocs[0].fromView, true, {viewDocs});
        const joinInputDocs = testDB.joinInput.find().toArray();

        // The $unionWith inside a $lookup inside $facet: the union must bring in only the
        // view's documents, with the view's pipeline applied.
        const result = testDB.source
            .aggregate([
                {
                    $facet: {
                        branch: [
                            {
                                $lookup: {
                                    from: testDB.joinInput.getName(),
                                    pipeline: [{$unionWith: "view"}],
                                    as: "outer",
                                },
                            },
                        ],
                    },
                },
            ])
            .toArray();

        assert.eq(result.length, 1, {result});
        const branchDocs = result[0].branch;
        assert.eq(branchDocs.length, 1, {result});

        // The $lookup sub-pipeline runs over joinInput's documents and takes the union with the view's,
        // so the result should be the joinInput documents plus the view's documents.
        const outerDocs = branchDocs[0].outer;
        assertArrayEq({actual: outerDocs, expected: joinInputDocs.concat(viewDocs)});
    });
});
