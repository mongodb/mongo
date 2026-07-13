/**
 * A bare `$lookup`/`$graphLookup`/`$unionWith` (no user `pipeline:`) over a view materializes the
 * foreign view's pipeline into the stage the first time involved namespaces are resolved. If the
 * aggregate's own main namespace is also a view and the request looks like it came from a router
 * (a `shardVersion`/`databaseVersion` attached but no pre-resolved view info, as an older router
 * would send), the shard resolves involved namespaces a second time from a copy of the first
 * resolution -- including the already-materialized subpipeline -- and must not try to materialize
 * it again.
 *
 * Each case below sends the aggregate directly to the shard mongod with a shardVersion attached
 * but no pre-resolved view info, which is exactly what an older (last-LTS) router would produce --
 * this deterministically triggers the shard's second resolution pass without needing a real
 * mixed-version cluster.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

function assertShardSurvives(shardDB, res) {
    // The command may succeed or fail (e.g. with a routing error asking to resolve the view
    // through mongos), but resolving involved namespaces for this request must not crash the
    // shard mongod.
    assert(typeof res.ok !== "undefined", tojson(res));

    // Confirm the shard mongod process is still alive and responsive.
    assert.commandWorked(shardDB.adminCommand({ping: 1}));
}

function runFromRouterlike(shardDB, mainNs, pipeline) {
    return shardDB.runCommand({
        aggregate: mainNs,
        pipeline,
        cursor: {},
        shardVersion: ShardVersioningUtil.kIgnoredShardVersion,
    });
}

describe("bare subpipeline stage over a chained view resolved twice", function () {
    const dbName = "test";
    const backingCollName = "backing";

    let st;
    let mongosDB;
    let shardDB;

    before(function () {
        st = new ShardingTest({shards: 1});
        mongosDB = st.s.getDB(dbName);
        shardDB = st.rs0.getPrimary().getDB(dbName);
        assert.commandWorked(mongosDB[backingCollName].insert({a: 1, b: 1}));
    });

    after(function () {
        st.stop();
    });

    it("does not crash the shard for a bare $lookup over a chained view", function () {
        const innerViewName = "innerView1";
        const lookupViewName = "lookupView1";
        const mainViewName = "mainView1";

        // Innermost view, targeted by lookupView's bare $lookup below.
        assert.commandWorked(
            mongosDB.createView(innerViewName, backingCollName, [{$match: {a: 1}}]),
        );

        // lookupView's own definition contains a bare $lookup (no user pipeline) into innerView.
        // This is the stage that materializes innerView's pipeline into itself the first time
        // lookupView gets resolved, and whose clone must not be re-materialized on the shard's
        // second resolution pass.
        assert.commandWorked(
            mongosDB.createView(lookupViewName, backingCollName, [
                {$lookup: {from: innerViewName, localField: "a", foreignField: "a", as: "out"}},
            ]),
        );

        // The aggregate's own main namespace must itself be a view to trigger the shard's second
        // resolution pass.
        assert.commandWorked(mongosDB.createView(mainViewName, backingCollName, [{$match: {}}]));

        // The top-level $lookup into lookupView has an explicit (empty) user pipeline, so it does
        // not itself materialize anything -- it only pulls lookupView into the
        // involved-namespaces resolution, which recursively resolves and materializes
        // lookupView's own bare $lookup into innerView.
        const res = runFromRouterlike(shardDB, mainViewName, [
            {
                $lookup: {
                    from: lookupViewName,
                    localField: "a",
                    foreignField: "a",
                    as: "out",
                    pipeline: [],
                },
            },
        ]);
        assertShardSurvives(shardDB, res);
    });

    it("does not crash the shard for a deeply nested view chain (view-on-view-on-view)", function () {
        const view1 = "deepView1";
        const view2 = "deepView2";
        const view3 = "deepView3";
        const lookupViewName = "deepLookupView";
        const mainViewName = "deepMainView";

        // view1 (backed directly by the collection) <- view2 (a view on view1) <- view3 (a view
        // on view2). Both the base and the foreign side of the bare $lookup below resolve through
        // this three-deep chain.
        assert.commandWorked(mongosDB.createView(view1, backingCollName, [{$match: {a: 1}}]));
        assert.commandWorked(mongosDB.createView(view2, view1, [{$match: {b: 1}}]));
        assert.commandWorked(mongosDB.createView(view3, view2, [{$match: {}}]));

        assert.commandWorked(
            mongosDB.createView(lookupViewName, view3, [
                {$lookup: {from: view3, localField: "a", foreignField: "a", as: "out"}},
            ]),
        );
        assert.commandWorked(mongosDB.createView(mainViewName, view3, [{$match: {}}]));

        const res = runFromRouterlike(shardDB, mainViewName, [
            {
                $lookup: {
                    from: lookupViewName,
                    localField: "a",
                    foreignField: "a",
                    as: "out",
                    pipeline: [],
                },
            },
        ]);
        assertShardSurvives(shardDB, res);
    });

    it("does not crash the shard for a self-$lookup (main namespace and foreign $lookup namespace are the same view)", function () {
        const selfViewName = "selfLookupView";

        // A view cannot reference itself in its own definition (view-cycle detection at create
        // time), so the "self" case here is the aggregate's main namespace and its bare $lookup's
        // foreign namespace being the same view -- resolving involved namespaces materializes
        // selfViewName's subpipeline while selfViewName is also the stage's own main/base
        // namespace binding.
        assert.commandWorked(
            mongosDB.createView(selfViewName, backingCollName, [{$match: {a: 1}}]),
        );

        const res = runFromRouterlike(shardDB, selfViewName, [
            {
                $lookup: {
                    from: selfViewName,
                    localField: "a",
                    foreignField: "a",
                    as: "out",
                    pipeline: [],
                },
            },
        ]);
        assertShardSurvives(shardDB, res);
    });

    it("does not crash the shard for a bare $unionWith over a chained view", function () {
        const innerViewName = "unionInnerView";
        const unionViewName = "unionMidView";
        const mainViewName = "unionMainView";

        assert.commandWorked(
            mongosDB.createView(innerViewName, backingCollName, [{$match: {a: 1}}]),
        );
        // A bare $unionWith (collection/view name only, no user pipeline) over innerView.
        assert.commandWorked(
            mongosDB.createView(unionViewName, backingCollName, [{$unionWith: innerViewName}]),
        );
        assert.commandWorked(mongosDB.createView(mainViewName, backingCollName, [{$match: {}}]));

        const res = runFromRouterlike(shardDB, mainViewName, [
            {$unionWith: {coll: unionViewName, pipeline: []}},
        ]);
        assertShardSurvives(shardDB, res);
    });

    it("does not crash the shard for a bare $graphLookup over a chained view", function () {
        const innerViewName = "graphInnerView";
        const graphViewName = "graphMidView";
        const mainViewName = "graphMainView";

        assert.commandWorked(
            mongosDB.createView(innerViewName, backingCollName, [{$match: {a: 1}}]),
        );
        // $graphLookup has no "pipeline:" option at all -- it is always a bare subpipeline-target
        // reference to the foreign namespace.
        assert.commandWorked(
            mongosDB.createView(graphViewName, backingCollName, [
                {
                    $graphLookup: {
                        from: innerViewName,
                        startWith: "$a",
                        connectFromField: "a",
                        connectToField: "a",
                        as: "out",
                    },
                },
            ]),
        );
        assert.commandWorked(mongosDB.createView(mainViewName, backingCollName, [{$match: {}}]));

        const res = runFromRouterlike(shardDB, mainViewName, [
            {
                $lookup: {
                    from: graphViewName,
                    localField: "a",
                    foreignField: "a",
                    as: "out",
                    pipeline: [],
                },
            },
        ]);
        assertShardSurvives(shardDB, res);
    });
});
