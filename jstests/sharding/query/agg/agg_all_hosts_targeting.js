/**
 * @tags: [
 *   # Prevent clustered collection passthrough, since we want untracked collection.
 *   expects_explicit_underscore_id_index,
 *   requires_sharding,
 *   requires_spawning_own_processes,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("kAllShardHosts followed by targeted-shard work", function () {
    let st;
    let db;

    before(function () {
        st = new ShardingTest({shards: 1, config: 1, rs: {nodes: 2}});
        db = st.s.getDB("test");

        // Collation for main collection.
        const caseInsensitive = {
            locale: "en_US",
            strength: 2,
        };

        assert.commandWorked(db.createCollection("main", {collation: caseInsensitive}));
        // This test expects that we have an untracked unsharded collection.
        assert.eq(null, st.config.collections.findOne({_id: "test.main"}));
        assert.commandWorked(db.createCollection("additional"));
        // Additional contains documents which are equivalent under case-insensitive collation.
        assert.commandWorked(db.additional.insertMany([{a: "AA"}, {a: "Aa"}, {a: "aa"}]));
    });

    after(function () {
        st.stop();
    });

    // Stages which need to run against a collection on all hosts (primary and secondary).
    const allShardHostsStages = [
        {$planCacheStats: {allHosts: true}},
        {$collStats: {count: {}, targetAllNodes: true}},
    ];

    // $unionWith is not allowed to merge on mongos - it must merge on a shard. Together with
    // a kAllShardHosts stage from above, we create a situation where we must run on all
    // hosts and we cannot merge on mongos, but the $unionWith which requires a shard does
    // not specify an explicit mergeShardId. Due to the untracked collection, we need to
    // query the collation from the primary during planning.
    function test(allShardHostsStage, finalStages, expectedMergeShardId) {
        const pipeline = [
            allShardHostsStage,
            {
                $unionWith: {
                    coll: "additional",
                    pipeline: [{$match: {a: "AA"}}],
                },
            },
            ...finalStages,
        ];
        const explain = assert.commandWorked(db.main.explain().aggregate(pipeline));
        assert.eq(expectedMergeShardId, explain.mergeShardId);

        const results = db.main.aggregate(pipeline).toArray();

        // The union pipeline should execute with the same collation as the main pipeline.
        // Since main's default collation is case-insensitive, the $match should return
        // all three strings.
        const unionResults = results.filter((result) => result.a);
        assert.sameMembers(
            ["AA", "Aa", "aa"],
            unionResults.map((result) => result.a),
        );
    }

    for (const stage of allShardHostsStages) {
        const stageName = Object.keys(stage)[0];
        // Case without adding a stage with mergeShardId.
        it(`${stageName} with shard targeting`, () => {
            test(stage, []);
        });
        // $lookup will set a mergeShardId.
        it(`${stageName} with shard targeting and stage setting mergeShardId`, () => {
            test(
                stage,
                [{$lookup: {from: "additional", as: "nested", pipeline: []}}],
                st.shard0.shardName,
            );
        });
    }
});
