/**
 * Asserts that plan shape counters in query stats increment correctly in both a standalone and a
 * sharded environment.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sharding,
 * ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getAggPlanStages, getEngine} from "jstests/libs/query/analyze_plan.js";
import {
    getQueryStats,
    resetQueryStatsStore,
    withQueryStatsEnabled,
} from "jstests/libs/query/query_stats_utils.js";
import {
    checkSbeStatus,
    isDeferredGetExecutorEnabled,
    kSbeRestricted,
} from "jstests/libs/query/sbe_util.js";

function runPlanShapeCounterTest(coll) {
    const deferredEngineChoiceEnabled = isDeferredGetExecutorEnabled(coll.getDB());
    const sbeStatus = checkSbeStatus(coll.getDB());

    // Plan shape counters are only populated on the deferred engine-selection path.
    // This test also asserts on the execution engine used.
    if (!deferredEngineChoiceEnabled || sbeStatus !== kSbeRestricted) {
        jsTest.log.info("Skipping plan shape counter test", {
            deferredEngineChoiceEnabled,
            sbeStatus,
        });
        return;
    }

    const statsConn = coll.getDB().getMongo();
    const isSharded = FixtureHelpers.isMongos(coll.getDB());

    assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}]));

    function getPlanShapeCounters(collName = coll.getName()) {
        const stats = getQueryStats(statsConn, {collName});
        assert.eq(1, stats.length, "expected exactly one query stats entry", {stats});
        const metrics = stats[0].metrics;
        const queryPlanner = metrics.hasOwnProperty("queryPlanner")
            ? metrics.queryPlanner
            : metrics;
        return queryPlanner.planShapeCounters;
    }

    function reset() {
        coll.dropIndexes();
        resetQueryStatsStore(statsConn, "1MB");
    }

    // Converts every value in '{[counterName]: [count]}' to a NumberLong.
    function convertObjToNumberLong(counters) {
        return Object.fromEntries(
            Object.entries(counters).map(([name, count]) => [name, NumberLong(count)]),
        );
    }

    function assertCounters(expected, collName = coll.getName()) {
        if (expected !== undefined) {
            if (expected.patterns) {
                expected.patterns = convertObjToNumberLong(expected.patterns);
            }
            if (expected.nodes) {
                expected.nodes = convertObjToNumberLong(expected.nodes);
            }
            if (expected.accessPaths) {
                expected.accessPaths = convertObjToNumberLong(expected.accessPaths);
            }
        }
        assert.eq(expected, getPlanShapeCounters(collName));
    }

    // An _id-based point query is answered by the express/idhack fast path, which matches no
    // tracked plan shape, so the planShapeCounters field is omitted entirely.
    {
        reset();
        coll.find({_id: 1}).itcount();
        assertCounters(undefined);
    }

    // A COLLSCAN-PROJECT plan. When sharded, the {a: 1} shard-key index answers this with a
    // covered IXSCAN-PROJECT plan on the single targeted shard.
    {
        reset();
        coll.find({a: 3}, {_id: 0, a: 1}).itcount();
        assertCounters(
            isSharded
                ? {
                      patterns: {ixscanProject: 1},
                      nodes: {ixscanNoFilter: 1, projectionCovered: 1},
                      accessPaths: {coveredIxscan: 1, btreeIxscan: 1, boundsPoint: 1},
                  }
                : {
                      patterns: {collscanProject: 1},
                      nodes: {collscanWithFilter: 1, projectionSimple: 1},
                      accessPaths: {collscan: 1},
                  },
        );
    }

    // A collection scan increments the COLLSCAN counter. When sharded the {a: 1} shard-key index
    // makes this an index scan instead, planned on both shards and summed by the router.
    {
        reset();
        coll.find({a: {$gte: 0}}).itcount();
        assertCounters(
            isSharded
                ? {
                      patterns: {ixscanFetch: 2},
                      nodes: {ixscanNoFilter: 2, fetchNoFilter: 2, shardingFilter: 2},
                      accessPaths: {ixscanFetch: 2, btreeIxscan: 2, boundsBoundedRange: 2},
                  }
                : {
                      patterns: {collscan: 1},
                      nodes: {collscanWithFilter: 1},
                      accessPaths: {collscan: 1},
                  },
        );
    }

    // Repeated executions of the same shape accumulate.
    {
        reset();
        for (let i = 0; i < 3; i++) {
            coll.find({a: {$gte: 0}}).itcount();
        }
        assertCounters(
            isSharded
                ? {
                      patterns: {ixscanFetch: 6},
                      nodes: {ixscanNoFilter: 6, fetchNoFilter: 6, shardingFilter: 6},
                      accessPaths: {ixscanFetch: 6, btreeIxscan: 6, boundsBoundedRange: 6},
                  }
                : {
                      patterns: {collscan: 3},
                      nodes: {collscanWithFilter: 3},
                      accessPaths: {collscan: 3},
                  },
        );
    }

    // A single query spanning multiple getMores is counted exactly once per shard.
    {
        reset();
        coll.find({a: {$gte: 0}})
            .batchSize(2)
            .itcount();
        assertCounters(
            isSharded
                ? {
                      patterns: {ixscanFetch: 2},
                      nodes: {ixscanNoFilter: 2, fetchNoFilter: 2, shardingFilter: 2},
                      accessPaths: {ixscanFetch: 2, btreeIxscan: 2, boundsBoundedRange: 2},
                  }
                : {
                      patterns: {collscan: 1},
                      nodes: {collscanWithFilter: 1},
                      accessPaths: {collscan: 1},
                  },
        );
    }

    // Multiple shapes work correctly.
    {
        reset();
        const query = () => coll.find({a: 3}).itcount();
        query();
        // Only 1 count in the sharded case, since this predicate matches the shard key.
        assertCounters(
            isSharded
                ? {
                      patterns: {ixscanFetch: 1},
                      nodes: {ixscanNoFilter: 1, fetchNoFilter: 1},
                      // The index bounds are a point, [3,3]
                      accessPaths: {ixscanFetch: 1, btreeIxscan: 1, boundsPoint: 1},
                  }
                : {
                      patterns: {collscan: 1},
                      nodes: {collscanWithFilter: 1},
                      accessPaths: {collscan: 1},
                  },
        );
        assert.commandWorked(coll.createIndex({a: 1}));
        query();
        // Now that an index exists, both ixscan bounds are a point [3,3]
        assertCounters(
            isSharded
                ? {
                      patterns: {ixscanFetch: 2},
                      nodes: {ixscanNoFilter: 2, fetchNoFilter: 2},
                      accessPaths: {ixscanFetch: 2, btreeIxscan: 2, boundsPoint: 2},
                  }
                : {
                      patterns: {collscan: 1, ixscanFetch: 1},
                      nodes: {collscanWithFilter: 1, ixscanNoFilter: 1, fetchNoFilter: 1},
                      accessPaths: {collscan: 1, ixscanFetch: 1, btreeIxscan: 1, boundsPoint: 1},
                  },
        );
    }

    // A COLLSCAN-SORT plan; when sharded, the shard-key index gives IXSCAN-FETCH-SORT on both
    // shards. Once the {a: 1, b: 1} index exists, the same query is answered with an
    // IXSCAN-SORT_MERGE-FETCH plan, accumulating alongside the earlier shape in the same entry.
    {
        reset();
        const query = () =>
            coll.aggregate([{$match: {a: {$in: [1, 2]}}}, {$sort: {b: 1}}]).itcount();
        query();
        assertCounters(
            isSharded
                ? {
                      patterns: {ixscanFetchSort: 2},
                      nodes: {
                          ixscanNoFilter: 2,
                          fetchNoFilter: 2,
                          shardingFilter: 2,
                          sortSimpleNoLimit: 2,
                      },
                      accessPaths: {
                          ixscanFetch: 2,
                          btreeIxscan: 2,
                          // The bounds are unioned points, [1,1]U[2,2]
                          boundsPoint: 2,
                          boundsUnionedSmall: 2,
                      },
                  }
                : {
                      patterns: {collscanSort: 1},
                      nodes: {collscanWithFilter: 1, sortSimpleNoLimit: 1},
                      accessPaths: {collscan: 1},
                  },
        );
        assert.commandWorked(coll.createIndex({a: 1, b: 1}));
        query();
        assertCounters(
            isSharded
                ? {
                      patterns: {ixscanFetchSort: 2, ixscanSortMergeFetch: 2},
                      nodes: {
                          ixscanNoFilter: 4,
                          fetchNoFilter: 4,
                          // sort_merge is used with <=100 children
                          sortMergeNoFilterLte100Children: 2,
                          shardingFilter: 4,
                          sortKeyGenerator: 2,
                          sortSimpleNoLimit: 2,
                      },
                      accessPaths: {
                          ixscanFetch: 4,
                          btreeIxscan: 4,
                          boundsPoint: 2,
                          // The bounds are now categorized as a mixture, since they
                          // are `{a: [1,1]U[2,2], b: [minkey, maxkey]}`
                          boundsMixture: 2,
                          boundsUnionedSmall: 2,
                      },
                  }
                : {
                      patterns: {collscanSort: 1, ixscanSortMergeFetch: 1},
                      nodes: {
                          collscanWithFilter: 1,
                          ixscanNoFilter: 1,
                          fetchNoFilter: 1,
                          sortMergeNoFilterLte100Children: 1,
                          sortSimpleNoLimit: 1,
                      },
                      accessPaths: {
                          collscan: 1,
                          ixscanFetch: 1,
                          btreeIxscan: 1,
                          boundsMixture: 1,
                      },
                  },
        );
    }

    // A bare IXSCAN (returnKey) matches no tracked plan shape, so the patterns category is
    // omitted, but the node and access path counters still report it.
    {
        reset();
        assert.commandWorked(coll.createIndex({a: 1}));
        coll.find({a: 3}).returnKey().itcount();
        assertCounters({
            nodes: {ixscanNoFilter: 1, returnKey: 1},
            accessPaths: {coveredIxscan: 1, btreeIxscan: 1, boundsPoint: 1},
        });
    }

    // Some queries ($lookup, $graphLookup, etc) access multiple collections. The expected
    // behavior is to only increment the counters for the local plan.

    // The $lookup cases below use their own local collection, which stays unsharded in both
    // fixtures. A sharded local collection would keep the $lookup on the merging node instead of
    // planning it on a shard, so neither the SBE lowering nor the classic inner-side planning that
    // these cases exercise would happen.
    const localColl = coll.getDB().lookupLocal;
    const foreignColl = coll.getDB().lookupForeign;
    assert(localColl.drop());
    assert(foreignColl.drop());
    assert.commandWorked(localColl.insert([{a: 1}, {a: 2}, {a: 3}]));
    assert.commandWorked(foreignColl.insert([{b: 1}, {b: 2}, {b: 3}]));
    assert.commandWorked(localColl.createIndex({z: 1}));

    // When $lookup is executed in SBE, plan shape counters should only be incremented for the local side.
    // There is no index on the foreign collection, so it's forced to do a collection scan. We'll use an
    // index scan on the local side so we can make sure the local side is being incremented, not the
    // foreign side.
    const localSideCollscanMetrics = {
        patterns: {ixscanFetch: 1},
        nodes: {ixscanNoFilter: 1, fetchNoFilter: 1},
        accessPaths: {ixscanFetch: 1, btreeIxscan: 1, boundsPoint: 1},
    };
    {
        const pipeline = [
            {$match: {z: 1}},
            {$lookup: {from: foreignColl.getName(), localField: "a", foreignField: "b", as: "c"}},
        ];
        const explain = localColl.explain().aggregate(pipeline);
        assert.eq("sbe", getEngine(explain), explain);

        // Assert the foreign side uses a collscan.
        const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");
        assert.eq(eqLookupNodes.length, 1, explain);
        assert.eq(eqLookupNodes[0].inputStages[1].stage, "COLLSCAN", explain);
        assert.eq(eqLookupNodes[0].inputStages[1].nss, foreignColl.getFullName(), explain);

        reset();
        localColl.aggregate(pipeline).toArray();
        assertCounters(
            {
                patterns: localSideCollscanMetrics.patterns,
                nodes: {...localSideCollscanMetrics.nodes, eqLookupNoUnwind: 1},
                accessPaths: localSideCollscanMetrics.accessPaths,
            },
            localColl.getName(),
        );
    }

    // When $lookup is executed in classic, plan shape counters should only be incremented for the local side.
    {
        const pipeline = [
            {$match: {z: 1}},
            {
                $lookup: {
                    from: foreignColl.getName(),
                    as: "c",
                    let: {localA: "$a"},
                    pipeline: [{$match: {$expr: {$eq: ["$b", "$$localA"]}}}, {$sort: {b: 1}}],
                },
            },
        ];
        const explain = localColl.explain().aggregate(pipeline);
        assert.eq("classic", getEngine(explain), explain);

        reset();
        localColl.aggregate(pipeline).toArray();
        assertCounters(localSideCollscanMetrics, localColl.getName());
    }

    // A $lookup nested inside the sub-pipeline of another $lookup. Neither level of foreign-side
    // planning should be counted.
    {
        reset();
        const pipeline = [
            {$match: {z: 1}},
            {
                $lookup: {
                    from: foreignColl.getName(),
                    as: "c",
                    pipeline: [{$lookup: {from: foreignColl.getName(), as: "d", pipeline: []}}],
                },
            },
        ];
        localColl.aggregate(pipeline).toArray();
        assertCounters(localSideCollscanMetrics, localColl.getName());
    }

    // $unionWith should only increment plan shape counters for the local side.
    {
        reset();
        const pipeline = [{$match: {z: 1}}, {$unionWith: {coll: foreignColl.getName()}}];
        localColl.aggregate(pipeline).toArray();
        assertCounters(localSideCollscanMetrics, localColl.getName());
    }

    // Same for a $unionWith with a non-trivial sub-pipeline.
    {
        reset();
        const pipeline = [
            {$match: {z: 1}},
            {$unionWith: {coll: foreignColl.getName(), pipeline: [{$sort: {b: 1}}]}},
        ];
        localColl.aggregate(pipeline).toArray();
        assertCounters(localSideCollscanMetrics, localColl.getName());
    }

    // $graphLookup executes its recursive lookups against the foreign collection, only the local
    // side should be counted.
    {
        reset();
        const pipeline = [
            {$match: {z: 1}},
            {
                $graphLookup: {
                    from: foreignColl.getName(),
                    startWith: "$a",
                    connectFromField: "b",
                    connectToField: "b",
                    as: "c",
                },
            },
        ];
        localColl.aggregate(pipeline).toArray();
        assertCounters(localSideCollscanMetrics, localColl.getName());
    }
}

withQueryStatsEnabled(jsTestName(), runPlanShapeCounterTest, {a: 1}, {a: 2});
