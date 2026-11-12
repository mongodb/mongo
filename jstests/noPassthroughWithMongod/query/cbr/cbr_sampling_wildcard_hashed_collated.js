/**
 * Test samplingCE on wildcard, hashed and collated indexes.
 *
 * These index types are estimated by generating the actual index keystrings from the sample
 * documents (their keys cannot be reproduced by projecting fields out of the documents).
 *  @tags:[
 *   featureFlagCostBasedRanker
 * ]
 */

import {
    getAllPlans,
    getPlanStages,
    getRejectedPlans,
    getWinningPlanFromExplain,
    ChosenRanker,
    assertChosenRanker,
    PlanRankerReason,
} from "jstests/libs/query/analyze_plan.js";
import {
    assertPlanCosted,
    getPlanRankerConfig,
    setPlanRankerConfig,
} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

// TODO SERVER-137714: Remove this exemption once CBR estimates are shown in explain output for
// SBE plans.
if (checkSbeFullyEnabled(db)) {
    jsTest.log.info(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const coll = db[jsTestName()];
const numDocs = 1000;

function defaultDoc(i) {
    return {
        _id: i,
        a: i % 20,
        c: {b: i % 5},
        s: i % 2 === 0 ? "ABC" : "abc",
    };
}

function resetColl(makeDoc = defaultDoc) {
    coll.drop();
    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push(makeDoc(i));
    }
    assert.commandWorked(coll.insertMany(docs));
}

const kCollation = {locale: "en", strength: 2};

const queries = [
    {
        name: "hashed equality",
        setup: () => {
            assert.commandWorked(coll.createIndex({a: "hashed"}));
        },
        query: {a: 5},
        expectedNumKeys: numDocs / 20,
        expectedNumDocs: numDocs / 20,
        expectedSeeks: 1,
    },
    {
        name: "wildcard equality",
        setup: () => {
            assert.commandWorked(coll.createIndex({"$**": 1}));
        },
        query: {"c.b": 3},
        expectedNumKeys: numDocs / 5,
        expectedNumDocs: numDocs / 5,
        expectedSeeks: 1,
    },
    {
        name: "wildcard exists (all documents have the path)",
        setup: () => {
            assert.commandWorked(coll.createIndex({"$**": 1}));
        },
        query: {"c.b": {$exists: true}},
        expectedNumKeys: numDocs,
        expectedNumDocs: numDocs,
        expectedSeeks: 2,
    },
    {
        name: "wildcard exists on a root field with a multi-leaf subtree",
        setup: () => {
            assert.commandWorked(coll.createIndex({"$**": 1}));
        },
        makeDocs: (i) => ({c: {a: 0, b: 0}}),
        query: {c: {$exists: true}},
        expectedNumKeys: numDocs * 2,
        expectedNumDocs: numDocs,
        expectedSeeks: 2,
    },
    {
        name: "wildcard exists (no document has the path)",
        setup: () => {
            assert.commandWorked(coll.createIndex({"$**": 1}));
        },
        query: {"c.a": {$exists: true}},
        expectedNumKeys: 0,
        expectedNumDocs: 0,
        expectedSeeks: 2,
    },
    {
        name: "wildcard equality (some documents have the path)",
        setup: () => {
            assert.commandWorked(coll.createIndex({"$**": 1}));
        },
        makeDocs: (i) => (i % 2 === 0 ? {c: {b: i % 5}} : {unrelated: i}),
        query: {"c.b": 3},
        expectedNumKeys: numDocs / 10,
        expectedNumDocs: numDocs / 10,
        expectedSeeks: 1,
    },
    {
        name: "wildcard exists (some documents have the path)",
        setup: () => {
            assert.commandWorked(coll.createIndex({"$**": 1}));
        },
        makeDocs: (i) => (i % 2 === 0 ? {c: {b: i % 5}} : {unrelated: i}),
        query: {"c.b": {$exists: true}},
        expectedNumKeys: numDocs / 2,
        expectedNumDocs: numDocs / 2,
        expectedSeeks: 2,
    },
    {
        name: "subtree wildcard with predicates on the subtree root and a nested path",
        setup: () => {
            assert.commandWorked(coll.createIndex({"m.$**": 1}));
        },
        query: {"m.m2": 0, "m": 0},
        makeDocs: (i) => ({m: {m2: i % 5}}),
        expectedNumKeys: 0,
        expectedNumDocs: 0,
        expectedSeeks: 1,
        expectedRejectedPlans: 1,
    },
    {
        name: "collated equality",
        setup: () => {
            assert.commandWorked(coll.createIndex({s: 1}, {collation: kCollation}));
        },
        query: {s: "abc"},
        collation: kCollation,
        expectedNumKeys: numDocs,
        expectedNumDocs: numDocs,
        expectedSeeks: 1,
    },
    {
        name: "compound wildcard prefix exists + equality on the trailing field",
        setup: () => {
            assert.commandWorked(coll.createIndex({"a.$**": 1, b: 1}));
        },
        makeDocs: (i) => (i % 4 === 3 ? {b: 0} : {a: {x: i % 5}, b: i % 2}),
        query: {a: {$exists: true}, b: 0},
        expectedNumKeys: numDocs / 2,
        expectedNumDocs: numDocs / 2,
        expectedSeeks: 5,
    },
    {
        name: "wildcard range over array values (one document, multiple keys)",
        setup: () => {
            assert.commandWorked(coll.createIndex({"$**": 1}));
        },
        makeDocs: (i) => ({c: {b: [i % 5, (i + 2) % 5]}}),
        query: {"c.b": {$gte: 0}},
        expectedNumKeys: numDocs * 2,
        expectedNumDocs: numDocs,
        expectedSeeks: 1,
    },
    {
        name: "collated equality over array values (both elements collate to the point)",
        setup: () => {
            assert.commandWorked(coll.createIndex({s: 1}, {collation: kCollation}));
        },
        makeDocs: (i) => ({s: ["ABC", "abc"]}),
        query: {s: "abc"},
        collation: kCollation,
        expectedNumKeys: numDocs,
        expectedNumDocs: numDocs,
        expectedSeeks: 1,
    },
    {
        name: "collated descending equality",
        setup: () => {
            assert.commandWorked(coll.createIndex({s: -1}, {collation: kCollation}));
        },
        query: {s: "abc"},
        collation: kCollation,
        expectedNumKeys: numDocs,
        expectedNumDocs: numDocs,
        expectedSeeks: 1,
    },
    {
        name: "compound collated point + range + range",
        setup: () => {
            assert.commandWorked(coll.createIndex({s: 1, a: 1, "c.b": 1}, {collation: kCollation}));
        },
        query: {s: "abc", a: {$lt: 10}, "c.b": {$lt: 5}},
        collation: kCollation,
        expectedNumKeys: numDocs / 2,
        expectedNumDocs: numDocs / 2,
        expectedSeeks: 10,
    },
    {
        name: "compound collated point + range + point",
        setup: () => {
            assert.commandWorked(coll.createIndex({s: 1, a: 1, "c.b": 1}, {collation: kCollation}));
        },
        query: {s: "abc", a: {$lt: 10}, "c.b": 0},
        collation: kCollation,
        expectedNumKeys: (numDocs * 2) / 20,
        expectedNumDocs: (numDocs * 2) / 20,
        expectedSeeks: 10,
    },
    {
        name: "multikey field not in the ndv",
        setup: () => {
            assert.commandWorked(coll.createIndex({a: 1, b: 1}, {collation: kCollation}));
        },
        makeDocs: (i) => ({a: i, b: [0, 1, 2, 3, 4]}),
        query: {a: {$gte: 0}, b: 0},
        collation: kCollation,
        expectedNumKeys: numDocs,
        expectedNumDocs: numDocs,
        // With `b` expanded, this uses Newton-Raphson and gets ~1007
        // for a complete sample over the 1000 documents in this test.
        // Without expanding `b`, it uses the fast path & gets the full
        // collection cardinality (exactly 1000).
        // The effect is more prominent with smaller sample sizes.
        expectedSeeks: numDocs,
    },
    {
        name: "Wildcard seek estimate must be based on conditional NDV",
        setup: () => {
            assert.commandWorked(coll.createIndex({"a.$**": 1, b: 1, c: 1}));
        },
        makeDocs: (i) => ({a: i % 3, b: i % 2, c: 0}),
        query: {a: {$lte: 1}, b: {$gte: 0}, c: 0},
        expectedNumKeys: 667,
        expectedNumDocs: 667,
        expectedSeeks: 4,
    },
];

function assertEstimatesCorrect({
    query,
    collation,
    expectedNumKeys,
    expectedNumDocs,
    expectedSeeks,
    expectedRejectedPlans = 0,
}) {
    let cmd = coll.find(query);
    if (collation !== undefined) {
        cmd = cmd.collation(collation);
    }
    const explain = cmd.explain("executionStats");

    assert.eq(getRejectedPlans(explain).length, expectedRejectedPlans, explain);

    if (expectedNumDocs === 0) {
        // 0-CE is treated as inestimable and falls back to MP.
        assertChosenRanker(
            explain,
            ChosenRanker.kMultiPlanning,
            PlanRankerReason.kCBRInestimableNode,
        );

        // Some plans may nevertheless be costed here.
    } else {
        for (const plan of getAllPlans(explain)) {
            assertPlanCosted(plan);

            assert.eq(plan.cardinalityEstimate, expectedNumDocs, plan);

            // The sample covers the whole collection, so the estimates are authoritative and tagged
            // 'Code' rather than 'Sampling'.
            assert.eq(plan.estimatesMetadata?.ceSource, "Code", plan);
        }
    }

    const plan = getWinningPlanFromExplain(explain);
    const ixstages = getPlanStages(plan, "IXSCAN");
    assert.eq(ixstages.length, 1, "Expected the winning plan to have one index scan", {
        plan,
    });
    const planStage = ixstages[0];
    if (typeof planStage.cardinalityEstimate !== "undefined") {
        assert.eq(planStage.cardinalityEstimate, expectedNumDocs, explain);
        assert.eq(planStage.numKeysEstimate, expectedNumKeys, explain);
        assert.eq(planStage.indexSeekEstimate, expectedSeeks, explain);
    } else {
        // We fall back to MP for zero-CE cases.
        // Then our expected values must also be zero.
        assert.eq(expectedNumDocs, 0, explain);
        // If we don't have a cardinalityEstimate, we should not have the
        // other estimates, either.
        assert.eq(typeof planStage.numKeysEstimate, "undefined", explain);
        assert.eq(typeof planStage.indexSeekEstimate, "undefined", explain);
    }

    const executionStages = getPlanStages(explain.executionStats.executionStages, "IXSCAN");
    assert.eq(executionStages.length, 1, explain);

    // keysExamined can count one extra key per seek.
    assert.gte(executionStages[0].keysExamined, expectedNumKeys, executionStages[0]);
    assert.lte(
        executionStages[0].keysExamined,
        expectedNumKeys + expectedSeeks,
        executionStages[0],
    );

    // The root estimate must match the number of returned documents.
    assert.eq(explain.executionStats.nReturned, expectedNumDocs, explain);
}

const config = getPlanRankerConfig(db);
const seqSampleBefore = assert.commandWorked(
    db.adminCommand({
        getParameter: 1,
        internalQuerySamplingBySequentialScan: 1,
    }),
).internalQuerySamplingBySequentialScan;

describe("samplingCE on wildcard, hashed and collated indexes", function () {
    before(function () {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryPlanRanker: "costBased",
                internalQueryCBRCEMode: "samplingCE",
                // Sequential sampling with a sample size covering the whole collection makes the
                // estimates deterministic and exact.
                internalQuerySamplingBySequentialScan: true,
                internalSamplingSizeOverride: numDocs,
            }),
        );
    });

    after(function () {
        setPlanRankerConfig(db, config);
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingBySequentialScan: seqSampleBefore,
            }),
        );
    });

    queries.forEach((indexType) => {
        it(`estimates ${indexType.name}`, function () {
            resetColl(indexType.makeDocs);
            indexType.setup();
            assertEstimatesCorrect(indexType);
        });
    });
});
