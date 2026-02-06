/**
 * The test asserts that eligible shapes run under SBE with EQ_LOOKUP_UNWIND, while ineligible
 * shapes either stay in SBE without EQ_LOOKUP_UNWIND or fall back to the classic engine.
 *
 * @tags: [
 *      requires_sbe,
 *      featureFlagSbeEqLookupUnwind,
 *      assumes_against_mongod_not_mongos,
 *      assumes_read_preference_unchanged,
 *      requires_pipeline_optimization,
 *      do_not_wrap_aggregations_in_facets,
 *  ]
 */

import {describe, it, before, after} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    planHasStage,
    aggPlanHasStage,
    getWinningPlanFromExplain,
    getAggPlanStage,
    getNestedProperties,
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestricted} from "jstests/libs/query/sbe_util.js";

if (!checkSbeRestricted(db)) {
    jsTest.log.info("Skipping test because SBE is not in restricted mode.");
    quit();
}

function seedCollection(collection, numDocs, fieldName) {
    const bulk = collection.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        const doc = {};
        doc[fieldName] = i;
        doc["arrayField"] = Array.from({length: 5}, () => Math.floor(Math.random() * numDocs));
        bulk.insert(doc);
    }
    assert.commandWorked(bulk.execute());
}

/**
 * Assert that neither the $lookup nor the $unwind stages are pushed down into SBE, and that the aggregation $lookup stage has absorbed the $unwind stage.
 */
function assertClassicLookupUnwind(explain) {
    // Assert that the $lookup stage is present in the classic aggregation plan.
    const lookup = getAggPlanStage(explain, "$lookup");
    assert(lookup, "Expected to find $lookup stage in the aggregation plan, explain: " + tojson(explain));

    // Assert that the $unwind stage was absorbed into the $lookup stage.
    assert(
        !getAggPlanStage(explain, "$unwind"),
        "Expected $unwind stage to be absorbed into $lookup stage, explain: " + tojson(explain),
    );
    assert.gt(
        getNestedProperties(lookup, "unwinding").length,
        0,
        "Expected $lookup stage to have unwinding property, explain: " + tojson(explain),
    );
}

/**
 * Assert that both the $lookup and $unwind stages are pushed down into SBE as a single EQ_LOOKUP_UNWIND stage.
 */
function assertSbeLookupUnwind(explain) {
    // Assert that neither the $lookup nor the $unwind stages are present in the aggregation plan.
    assert(!aggPlanHasStage(explain, "$lookup"), `Expected $lookup to be pushed down, explain: ${tojson(explain)}`);
    assert(!aggPlanHasStage(explain, "$unwind"), `Expected $unwind to be pushed down, explain: ${tojson(explain)}`);

    // Assert that there is a single EQ_LOOKUP_UNWIND stage in the winning SBE plan.
    const queryPlan = getWinningPlanFromExplain(explain);
    assert(
        planHasStage(db, queryPlan, "EQ_LOOKUP_UNWIND"),
        `Expected plan to have stage EQ_LOOKUP_UNWIND, explain: ${tojson(queryPlan)}`,
    );
}

/**
 * Asserts that the $lookup stage is present in the SBE query plan, while the $unwind stage is present in the aggregation plan.
 */
function assertSbeLookupClassicUnwind(explain) {
    // Assert that the lookup stage is present in the SBE aggregation plan.
    assert(!aggPlanHasStage(explain, "$lookup"), `Expected $lookup to be in SBE, explain: ${tojson(explain)}`);
    const queryPlan = getWinningPlanFromExplain(explain);
    assert(
        planHasStage(db, queryPlan, "EQ_LOOKUP"),
        `Expected plan to have stage LOOKUP, explain: ${tojson(queryPlan)}`,
    );

    // Assert that the unwind stage is present in the classic aggregation plan.
    assert(aggPlanHasStage(explain, "$unwind"), `Expected $unwind to be in classic, explain: ${tojson(explain)}`);
}

describe("$LU pushdown", function () {
    before(function () {
        this.collection = assertDropAndRecreateCollection(db, jsTestName());
        this.foreignCollection = assertDropAndRecreateCollection(db, jsTestName() + "_foreign");
        seedCollection(this.collection, 100, "a");
        seedCollection(this.foreignCollection, 100, "b");
    });

    after(function () {
        assert(this.collection.drop());
        assert(this.foreignCollection.drop());
    });

    it("Should pushdown lookup-unwind", function () {
        const explain = this.collection
            .explain()
            .aggregate([{$lookup: {from: "foreign", localField: "a", foreignField: "b", as: "c"}}, {$unwind: "$c"}]);
        assertSbeLookupUnwind(explain);
    });

    it("Should not pushdown unabsorbed unwinds", function () {
        const explain = this.collection
            .explain()
            .aggregate([
                {$lookup: {from: "foreign", localField: "a", foreignField: "b", as: "c"}},
                {$unwind: "$arrayField"},
            ]);
        assertSbeLookupClassicUnwind(explain);
    });

    it("Should pushdown when preserveNullAndEmptyArrays is set to true", function () {
        const explain = this.collection
            .explain()
            .aggregate([
                {$lookup: {from: "foreign", localField: "a", foreignField: "b", as: "c"}},
                {$unwind: {path: "$c", preserveNullAndEmptyArrays: true}},
            ]);
        assertSbeLookupUnwind(explain);
    });

    it("Should not pushdown when includeArrayIndex is set", function () {
        const explain = this.collection
            .explain()
            .aggregate([
                {$lookup: {from: "foreign", localField: "a", foreignField: "b", as: "c"}},
                {$unwind: {path: "$c", includeArrayIndex: "idx"}},
            ]);
        assertClassicLookupUnwind(explain);
    });

    it("Should not pushdown if a project is between lookup and unwind", function () {
        const explain = this.collection
            .explain()
            .aggregate([
                {$lookup: {from: "foreign", localField: "a", foreignField: "b", as: "c"}},
                {$project: {other: 1}},
                {$unwind: "$c"},
            ]);
        assertSbeLookupClassicUnwind(explain);
    });

    it("Should not pushdown pipeline-based lookup with unwind", function () {
        const explain = this.collection.explain().aggregate([
            {
                $lookup: {
                    from: "foreign",
                    let: {aVar: "$a"},
                    pipeline: [{$match: {$expr: {$gt: ["$b", "$$aVar"]}}}],
                    as: "c",
                },
            },
            {$unwind: "$c"},
        ]);
        assertClassicLookupUnwind(explain);
    });

    it("Should not push down lookup-unwind when internalQuerySlotBasedExecutionDisableLookupUnwindPushdown is set", function () {
        const setDisablePushdown = (value) =>
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQuerySlotBasedExecutionDisableLookupUnwindPushdown: value}),
            );
        const explain = (() => {
            try {
                setDisablePushdown(true);
                return this.collection
                    .explain()
                    .aggregate([
                        {$lookup: {from: "foreign", localField: "a", foreignField: "b", as: "c"}},
                        {$unwind: "$c"},
                    ]);
            } finally {
                setDisablePushdown(false);
            }
        })();
        assertClassicLookupUnwind(explain);
    });
});
