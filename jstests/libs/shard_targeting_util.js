// Contains utilities and helper functions for testing shard targeting of aggregate commands.
import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {getAggPlanStage} from "jstests/libs/analyze_plan.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

/**
 * Class which allows for setting up a test fixture to test the behavior of shard targeting for
 * aggregate commands.
 */
export class ShardTargetingTest {
    constructor(db, shardDBMap) {
        this.db = db;
        this.shardProfileDBMap = shardDBMap;
        this.shardDBList = [];
        for (const [_, shardDB] of Object.entries(this.shardProfileDBMap)) {
            this.shardDBList.push(shardDB);
        }
    }

    // Helper functions.

    /**
     * Utility to clear the profiling collection.
     */
    _resetProfiling() {
        assert(this.shardDBList, "shardDBList must be defined");
        for (const shardDB of this.shardDBList) {
            assert.commandWorked(shardDB.setProfilingLevel(0));
            shardDB.system.profile.drop();
            assert.commandWorked(shardDB.setProfilingLevel(2));
        }
    }

    /**
     * Utility which asserts that the aggregation stages in 'actualStages' match those in
     * 'expectedStages'.
     */
    _assertExpectedStages(expectedStages, actualStages, explain) {
        assert.eq(expectedStages.length, actualStages.length, explain);
        let stageIdx = 0;
        for (const stage of expectedStages) {
            const spec = actualStages[stageIdx];
            assert(spec.hasOwnProperty(stage),
                   "Expected stage " + tojson(stage) + " in explain " + tojson(explain));
            stageIdx++;
        }
    }

    /**
     * Utility to create a filter for querying the database profiler with the provided parameters.
     */
    _createProfileFilter({ns, comment, expectedStages}) {
        let profileFilter = {"op": "command", "command.aggregate": ns};
        if (comment) {
            profileFilter["command.comment"] = comment;
        }
        let idx = 0;
        for (const stage of expectedStages) {
            const fieldName = "command.pipeline." + idx + "." + stage;
            profileFilter[fieldName] = {"$exists": true};
            idx++;
        }

        // If we have an empty list of stages, this may indicate a cursor established by a
        // $mergeCursors stage internally. As such, we wish to verify a cursor was established,
        // regardless of how many stages were specified.
        if (expectedStages) {
            profileFilter["command.pipeline"] = {"$size": expectedStages.length};
        }
        return profileFilter;
    }

    _examineSplitPipeline({splitPipeline, expectedMergingStages, expectedShardStages}) {
        if (splitPipeline.mergerPart) {
            const mergerPart = splitPipeline.mergerPart;
            if (mergerPart.length > 0) {
                assert(
                    expectedMergingStages,
                    "Should have specified merging stages for test case if split pipeline has 'mergerPart'" +
                        tojson(splitPipeline));
                this._assertExpectedStages(expectedMergingStages, mergerPart, splitPipeline);
            }
        }

        if (splitPipeline.shardsPart) {
            const shardsPart = splitPipeline.shardsPart;
            if (shardsPart.length > 0) {
                assert(
                    expectedShardStages,
                    "Should have specified shard stages for test case if split pipeline has 'shardsPart'" +
                        tojson(splitPipeline));
                this._assertExpectedStages(expectedShardStages, shardsPart, splitPipeline);
            }
        }
    }

    /**
     * Utility which makes certain assertions about 'explain' (obtained by running 'explain'),
     * namely:
     * - 'expectedMergingShard' and 'expectedMergingStages' allow for assertions around the shard
     * which was chosen as the merger and what pipeline is used to merge.
     * - 'expectedShard' and 'expectedShardStages' allow for assertions around targeting a single
     * shard for execution.
     * - 'assertSBELookupPushdown' asserts that $lookup was pushed down into SBE when present.
     */
    _assertExplainTargeting(explain, {
        expectedMergingShard,
        expectedMergingStages,
        expectedShard,
        expectedShardStages,
        assertSBELookupPushdown,
        expectMongos,
    }) {
        if (expectedMergingShard) {
            assert.eq(explain.mergeType, "specificShard", explain);
            assert.eq(explain.mergeShardId, expectedMergingShard, explain);
            assert(explain.hasOwnProperty("splitPipeline"), explain);
            this._examineSplitPipeline({
                splitPipeline: explain.splitPipeline,
                expectedMergingStages: expectedMergingStages,
                expectedShardStages: expectedShardStages
            });
        } else {
            assert.neq(explain.mergeType,
                       "specificShard",
                       "Expected not to merge on a specific shard; explain " + tojson(explain));
            assert.neq(
                explain.mergeType, "anyShard", "Expected not to merge on any shard", explain);
        }

        if (expectedShard) {
            assert(explain.hasOwnProperty("shards"), explain);
            const shards = explain.shards;
            const keys = Object.keys(shards);
            assert.eq(keys.length, 1, explain);
            assert.eq(expectedShard, keys[0], explain);

            const shard = shards[expectedShard];
            if (expectedShardStages) {
                const stages = shard.stages;
                assert(stages, explain);
                this._assertExpectedStages(expectedShardStages, stages, explain);
            }

            const stage = getAggPlanStage(shard, "EQ_LOOKUP", true /* useQueryPlannerSection */);
            if (assertSBELookupPushdown) {
                assert.neq(stage, null, shard);
            } else {
                assert.eq(stage, null, shard);
            }
        }

        if (expectMongos) {
            assert.eq(explain.mergeType, "mongos", explain);
            assert(explain.hasOwnProperty("splitPipeline"), explain);
            this._examineSplitPipeline({
                splitPipeline: explain.splitPipeline,
                expectedMergingStages: expectedMergingStages,
                expectedShardStages: expectedShardStages
            });
        }
    }

    // Testing functions.

    /**
     * Function to set up a collection in 'db':
     * - 'collName' specifies the name of the collection.
     * - 'indexList' specifies a list of indexes to build on the collection.
     * - 'docs' specifies a set of documents to insert.
     * - 'collType' specifies the type of collection (i.e. "sharded" or "unsplittable").
     * - 'shardKey' and 'chunkList' are used to configure a sharded collection.
     * - 'owningShard' designates the shard that an unsplittable collection should live on.
     */
    setupColl({collName, indexList, docs, collType, shardKey, chunkList, owningShard}) {
        const coll = this.db[collName];
        if (collType === "sharded") {
            assert(shardKey && chunkList,
                   "Must specify shard key and chunk list when setting up a sharded collection");
            CreateShardedCollectionUtil.shardCollectionWithChunks(coll, shardKey, chunkList);
        } else if (collType == "unsplittable") {
            assert(owningShard,
                   "Must specify an owning shard when setting up an unsplittable collection");
            // TODO (SERVER-85395) Replace createUnsplittableCollection with create command once
            // featureFlagTrackUnshardedCollectionsUponCreation is enabled.
            assert.commandWorked(this.db.runCommand(
                {createUnsplittableCollection: collName, dataShard: owningShard}));
        } else {
            assert(false, "Unknown collection type " + tojson(collType));
        }

        if (indexList && indexList.length > 0) {
            assert.commandWorked(coll.createIndexes(indexList));
        }

        if (docs && docs.length > 0) {
            assert.commandWorked(coll.insertMany(docs));
        }
    }

    /**
     * Function which runs 'pipeline' using the explain and aggregate commands to verify correct
     * results and expected shard targeting behavior.
     * - 'targetCollName' names the collection to target 'pipeline' with.
     * - 'explainAssertionObj' describes assertions to be made against the explain output (see
     *   'assertExplainTargeting' for more detail).
     * - 'expectedResults' contains the output that running 'pipeline' should produce.
     * - 'comment' is a string that will allow 'pipeline' (and, in some cases, sub-queries) to be
     *    uniquely identified in profiler output.
     * - 'profileFilters' is a map from shard name to objects containing arguments to create a
     * filter to query the profiler output.
     */
    assertShardTargeting({
        pipeline,
        targetCollName,
        explainAssertionObj,
        expectedResults,
        comment,
        profileFilters,
    }) {
        const coll = this.db[targetCollName];

        const options = comment ? {'comment': comment} : {};

        // Test explain if 'explainAssertionObj' is specified.
        if (explainAssertionObj) {
            const explain = coll.explain().aggregate(pipeline, options);
            this._assertExplainTargeting(explain, explainAssertionObj);
        }

        // Always reset the profiling collections before running an aggregate.
        this._resetProfiling();

        // Verify that 'pipeline' returns the expected results.
        const res = coll.aggregate(pipeline, options).toArray();
        assert(arrayEq(res, expectedResults),
               "sharded aggregation results did not match: " + tojson(res) +
                   " does not have the same members as " + tojson(expectedResults));

        // Verify that execution targeted the expected nodes if 'profileFilters' was specified.
        if (profileFilters) {
            for (const [shard, filterList] of Object.entries(profileFilters)) {
                const profileDB = this.shardProfileDBMap[shard];
                assert(profileDB);
                for (let filter of filterList) {
                    filter.comment = comment;
                    const profileFilter = this._createProfileFilter(filter);
                    const profileColl = profileDB.system.profile;
                    const debugFilter = comment ? {"command.comment": comment} : {};
                    assert.gt(profileColl.find(profileFilter).itcount(),
                              0,
                              "Expected to find an entry matching " + tojson(profileFilter) +
                                  " on shard " + shard +
                                  ". Dumping profiler contents limited to filter " +
                                  tojson(debugFilter) + ": " +
                                  tojson(profileColl.find(debugFilter).toArray()));
                }
            }
        }
    }
}
