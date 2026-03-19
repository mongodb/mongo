/**
 * Tests that aggregation correctly applies all of the view pipelines when the view definition
 * changes mid-command. Specifically, verifies that if 2 view kickbacks occur both view definitions
 * are applied to the returning documents. 2 view kickbacks can occur when the backing collection for
 * one view is dropped and recreated as a view mid aggregation.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

describe("View pipeline should apply when the collections change during aggregation", function () {
    let st;
    let mongos;
    let db;
    let unshardedColl;
    let shardedColl;
    const viewName = "myView";
    let shardedCollWithView;

    before(function () {
        st = new ShardingTest({shards: 2, mongos: 1});
        mongos = st.s;
        const dbName = jsTestName();
        db = mongos.getDB(dbName);

        assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    });

    beforeEach(function () {
        // Reset all of the collections.
        // Unsharded lives on the primary shard.
        unshardedColl = db.unsharded;
        unshardedColl.drop();
        assert.commandWorked(
            unshardedColl.insertMany([
                {_id: 1, key: "A"},
                {_id: 2, key: "B"},
                {_id: 3, key: "A"},
            ]),
        );

        // Sharded across both shards.
        shardedColl = db.shardedColl;
        shardedColl.drop();
        assert.commandWorked(shardedColl.createIndex({active: 1}));
        assert.commandWorked(
            shardedColl.insertMany([
                {_id: 1, val: "A", color: "red", active: true},
                {_id: 2, val: "B", color: "red", active: true},
                {_id: 3, val: "A", color: "blue", active: false},
            ]),
        );
        assert.commandWorked(st.s.adminCommand({shardCollection: shardedColl.getFullName(), key: {active: 1}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: shardedColl.getFullName(),
                find: {active: true},
                to: st.shard1.shardName,
            }),
        );

        // Sharded across both shards. Will be dropped and recreated by the test.
        shardedCollWithView = db.shardedCollWithView;
        shardedCollWithView.drop();
        assert.commandWorked(shardedCollWithView.createIndex({color: 1}));
        assert.commandWorked(
            shardedCollWithView.insertMany([
                {_id: 1, val: "A", color: "red"},
                {_id: 2, val: "B", color: "red"},
                {_id: 3, val: "B", color: "blue"},
                {_id: 4, val: "A", color: "blue"},
                {_id: 5, val: "A", color: "blue"},
            ]),
        );
        assert.commandWorked(st.s.adminCommand({shardCollection: shardedCollWithView.getFullName(), key: {color: 1}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: shardedCollWithView.getFullName(),
                find: {color: "blue"},
                to: st.shard1.shardName,
            }),
        );

        // View on a sharded collection to raise 'CommandOnShardedViewNotSupportedOnMongod'.
        db[viewName].drop();
        assert.commandWorked(db.createView(viewName, shardedCollWithView.getName(), [{$addFields: {"oldView": 2}}]));
    });

    after(function () {
        st.stop();
    });

    /**
     * Runs an aggregation on `viewName` with `pipeline` through a failpoint that pauses mongos
     * before retrying after the first view kickback. While paused, calls `mutate` to drop and
     * recreate the backing collection. Asserts the final result equals `expectedResult`.
     */
    function runTestWithFailPoint({pipeline, mutate, expectedResult}) {
        const fp = configureFailPoint(st.s, "hangBeforeRetryingAggregateAfterViewKickback");
        const runAgg = startParallelShell(
            funWithArgs(
                function (dbName, viewName, pipeline, expectedResult) {
                    const testDB = db.getSiblingDB(dbName);
                    const result = testDB[viewName].aggregate(pipeline).toArray();
                    assert.sameMembers(result, expectedResult);
                },
                db.getName(),
                viewName,
                pipeline,
                expectedResult,
            ),
            st.s.port,
        );
        fp.wait();
        mutate();
        fp.off();
        runAgg();
    }

    it("On mongos with simple pipeline", function () {
        // Simple pipeline that matches on 'val'. After the failpoint the result should include
        // fields added by the new view definition.
        runTestWithFailPoint({
            pipeline: [{$match: {"val": "A"}}],
            mutate: () => {
                shardedCollWithView.drop();
                assert.commandWorked(
                    db.createView(shardedCollWithView.getName(), shardedColl.getName(), [
                        {$addFields: {"newField": 10}},
                    ]),
                );
            },
            expectedResult: [
                {_id: 1, val: "A", color: "red", active: true, oldView: 2, newField: 10},
                {_id: 3, val: "A", color: "blue", active: false, oldView: 2, newField: 10},
            ],
        });
    });

    it("On mongos with views with a collation", function () {
        // Ensure the collation is applied in the query.
        // Drop the view created in beforeEach and recreate it with a case-insensitive collation.
        db[viewName].drop();
        assert.commandWorked(
            db.createView(viewName, shardedCollWithView.getName(), [{$addFields: {"oldView": 2}}], {
                collation: {locale: "en", strength: 2},
            }),
        );

        // Match on lowercase "a" — with the case-insensitive collation this should match documents
        // where val is "A". Without the collation the match would return nothing.
        runTestWithFailPoint({
            pipeline: [{$match: {"val": "a"}}],
            mutate: () => {
                shardedCollWithView.drop();
                assert.commandWorked(
                    db.createView(
                        shardedCollWithView.getName(),
                        shardedColl.getName(),
                        [{$addFields: {"newField": 10}}],
                        {collation: {locale: "en", strength: 2}},
                    ),
                );
            },
            // The case-insensitive collation is preserved across the view kickback:
            // "a" matches "A", and both the old and new view pipelines are applied.
            expectedResult: [
                {_id: 1, val: "A", color: "red", active: true, newField: 10, oldView: 2},
                {_id: 3, val: "A", color: "blue", active: false, newField: 10, oldView: 2},
            ],
        });
    });
});
