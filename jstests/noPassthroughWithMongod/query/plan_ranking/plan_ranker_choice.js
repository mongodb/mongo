/**
 * Verifies which plan ranker (the cost-based ranker, CBR, or the multi-planner, MP) produces the
 * winning plan, and the reason, across the AutomaticCE plan-ranking strategies:
 *   - EstimateRankingEffort:     after a brief MP estimation trial, CBR is chosen when it is
 *                                estimated cheaper than finishing MP.
 *   - NoMultiplanningResults:    CBR is engaged only when MP produced no results within its
 *                                trial budget.
 *
 * Each case documents its expected chosenRanker/reason and asserts it via assertChosenRanker().
 */
import {
    assertChosenRanker,
    ChosenRanker,
    PlanRankerReason,
} from "jstests/libs/query/analyze_plan.js";
import {
    getExpectedWorksPerPlan,
    getMultiplanningBatchSize,
    getPlanRankerConfig,
} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {LcgRandom} from "jstests/libs/lcg_random.js";

// TODO SERVER-130973 Enable CBR tests against trySbeEngine
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

function collName(collSufix) {
    return jsTestName() + "_" + collSufix;
}

function populateCollection(collSufix, cardinality, numFields, compoundIndexes) {
    // Generate a repeatable pseudo-random number sequence.
    const rng = new LcgRandom(314159265359);
    const coll = db[collName(collSufix)];
    coll.drop();

    const batchSize = 1000;
    let ops = [];
    let ndvs = []; // One NDV per field to use for data generation.
    // The portion of NDV that each field's NDV is different from the next field. The dividend is
    // a number < 1.0 so that even the last field gets an NDV that is < collection cardinality.
    const ndvStep = 0.9 / numFields;
    for (let j = numFields; j > 0; j--) {
        const curNDV = cardinality * j * ndvStep;
        ndvs.push(curNDV);
    }

    for (let i = 0; i < cardinality; i++) {
        let doc = {_id: i};
        for (let f = 1; f <= numFields; f++) {
            doc[`f${f}`] = rng.getRandomInt(0, ndvs[f - 1]);
        }
        // Extra non-indexed fields. Their values are non-random in order to provide deterministic
        // control of predicate selectivity and productivity
        doc["x1"] = i % 100;
        doc["x2"] = i % 1000;
        doc["s1"] = "txt_" + doc["f1"];
        doc["s2"] = "txt_" + doc["f2"];
        ops.push({
            insertOne: {document: doc},
        });

        // Execute and clear batch when full
        if (ops.length === batchSize || i === cardinality - 1) {
            assert.commandWorked(coll.bulkWrite(ops));
            ops = [];
        }
    }

    // Create indexes for f1...f<numFields>
    for (let f = 1; f <= numFields; f++) {
        let indexSpec = {};
        indexSpec[`f${f}`] = 1;
        assert.commandWorked(coll.createIndex(indexSpec));
    }

    for (let idx of compoundIndexes) {
        assert.commandWorked(coll.createIndex(idx));
    }
}

const compoundIndexes = [
    {f1: 1, f2: 1, f3: 1},
    {f4: 1, f5: 1, f3: 1},
    {f5: 1, f4: 1, f3: 1, f2: 1, f1: 1},
    {s1: "text"},
    {s2: 1},
];

const nFields = Math.max(...compoundIndexes.map((obj) => Object.keys(obj).length));
const batchSize = getMultiplanningBatchSize();

function checkRanker({
    qID = "",
    cName = "",
    query = {},
    order = {},
    limit = 0,
    returnKey = false,
    chosenRanker = "",
    reason = "",
}) {
    const coll = db[collName(cName)];

    let cursor = coll.find(query).sort(order);
    if (limit > 0) {
        cursor = cursor.limit(limit);
    }
    if (returnKey) {
        cursor = cursor.returnKey();
    }
    jsTest.log.info(`Testing case: ${qID}`, {chosenRanker, reason});
    const explain = assert.commandWorked(cursor.explain("allPlansExecution"));
    assertChosenRanker(explain, {chosenRanker, reason});
}

populateCollection("100", 100, nFields, compoundIndexes);
populateCollection("20k", 20000, nFields, compoundIndexes);

const prevCBRConfig = getPlanRankerConfig(db);
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        featureFlagCostBasedRanker: true,
        internalQueryPlanRanker: "mixed",
        internalQueryCBRCEMode: "samplingCE",
        internalQueryMixedPlanRankingStrategy: "EstimateRankingEffort",
    }),
);
try {
    // The implementation of AutomaticCE with a cost-based choice of the plan ranker
    // considers 5 different cases. Each of these cases is listed below and tested.

    // PLEASE MAKE SURE TO TEST ALL CASES, AND TO MATCH THE NUMBERS HERE WITH THE
    // ENUMERATION OF CASES IN THE CODE.

    // (1) AutomaticCE chooses MP because of EOF or full batch
    // 1.1 EOF small collection
    checkRanker({
        qID: "1.1.1",
        cName: "100",
        query: {f1: {$gte: 0}, f2: {$lte: 0}},
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kMpEarlyExitEofOrFullBatch,
    });
    checkRanker({
        qID: "1.1.2",
        cName: "100",
        query: {f1: {$gte: 0}, f2: {$lte: 0}},
        order: {f1: 1, x1: 1},
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kMpEarlyExitEofOrFullBatch,
    });
    checkRanker({
        qID: "1.1.3",
        cName: "100",
        query: {
            $and: [
                {
                    $or: [
                        {f1: {$gte: 100}},
                        {f2: {$lte: 901}},
                        {f3: {$gte: 50}},
                        {f4: {$lte: 990}},
                        {f5: {$gte: 10}},
                    ],
                },
                {
                    $or: [
                        {f1: {$lte: 150}},
                        {f2: {$gte: 910}},
                        {f3: {$lte: 250}},
                        {f4: {$gte: 870}},
                        {f5: {$lte: 25}},
                    ],
                },
            ],
        },
        order: {f3: 1, f1: 1},
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kMpEarlyExitEofOrFullBatch,
    });
    // 1.2 EOF big collection
    checkRanker({
        qID: "1.2.1",
        cName: "20k",
        query: {f1: 500, f2: {$gt: 300}},
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kMpEarlyExitEofOrFullBatch,
    });
    checkRanker({
        qID: "1.2.2",
        cName: "20k",
        query: {f1: {$in: [500, 600]}, f2: {$gt: 100}},
        order: {f1: 1},
        limit: batchSize,
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kMpEarlyExitEofOrFullBatch,
    });

    // 1.3 full batch
    checkRanker({
        qID: "1.3.1",
        cName: "20k",
        query: {f1: {$lt: 505}, f2: {$gt: 990}},
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kMpEarlyExitEofOrFullBatch,
    });
    checkRanker({
        qID: "1.3.2",
        cName: "20k",
        query: {f1: {$lt: 505}, f2: {$lt: 200}},
        order: {f3: 1},
        limit: batchSize + 1,
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kMpEarlyExitEofOrFullBatch,
    });

    // (2) "AutomaticCE chooses MP because plan contains inestimable node(s)"
    // $text creates inestimable TEXT stages but also forces the text index, so a plain conjunction
    // would produce only one plan. Using $or lets branches be planned independently: the $text
    // branch uses the text index while other branches have competing index choices, giving us
    // multiple candidate plans that all contain inestimable nodes.
    const queryWithCBRInestimableNodes = {
        "$or": [
            {"f1": {"$ne": 72}},
            {"s2": {"$regex": "text_"}},
            {"$text": {"$search": "text_77"}},
        ],
    };
    const queryWithCBRInestimableNodesSort = {"_id": 1};

    checkRanker({
        qID: "2.1",
        cName: "20k",
        query: queryWithCBRInestimableNodes,
        order: queryWithCBRInestimableNodesSort,
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kSinglePlan,
    });

    // When case 2 falls back to MP, the remaining trials must run so the plan is cached with a
    // sufficient number of works (not just the brief estimation phase works).
    {
        const coll20k = db[collName("20k")];

        // This sub-test reads the plan cache, so ensure it is enabled.
        const prevDisablePlanCache = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryDisablePlanCache: 1}),
        ).internalQueryDisablePlanCache;
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryDisablePlanCache: false}),
        );

        try {
            // The first $or branch ({f1: {$ne: 72}}) has 2 candidate plans.
            const expectedWorks = getExpectedWorksPerPlan(db, coll20k, 2);

            coll20k.getPlanCache().clear();
            coll20k
                .find(queryWithCBRInestimableNodes)
                .sort(queryWithCBRInestimableNodesSort)
                .toArray();

            const entries = coll20k.getPlanCache().list();
            // Only the first $or branch goes through multi-planning, producing the single cache
            // entry.
            assert.eq(
                entries.length,
                1,
                "Expected exactly one plan cache entry. " + tojson(entries),
            );

            const entry = entries[0];
            assert.eq(entry.isActive, false);
            assert.eq(
                entry.works,
                expectedWorks,
                "Plan cache entry works should equal the full MP trial budget. " +
                    "Got works=" +
                    entry.works +
                    ", expected=" +
                    expectedWorks,
            );
        } finally {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryDisablePlanCache: prevDisablePlanCache,
                }),
            );
        }
    }

    // 2.2 A case where the cost-based-ranker-choice strategy is entered (there are multiple
    // candidate plans, so it runs the brief MP estimation trial to compare MP against CBR) but a
    // plan contains an inestimable node. A '$near' predicate forces a GEO_NEAR_2DSPHERE stage,
    // which neither the exact CE used to estimate MP nor CBR can estimate, so estimateAllPlans()
    // fails and AutomaticCE falls back to MP. Uses a dedicated geo collection so the shared 'f1..'
    // collections (and the finely-tuned productivity cases) are left untouched.
    {
        const geoColl = db[collName("geo")];
        geoColl.drop();
        let ops = [];
        const numDocs = 20000;
        for (let i = 0; i < numDocs; i++) {
            ops.push({
                insertOne: {
                    document: {
                        _id: i,
                        // Deterministic, non-selective values: 'f1 > 100' matches ~90% of docs, so
                        // the inestimable $near geo plan (not 'f1') drives the query. The exact
                        // distribution is unimportant for this test.
                        f1: i % 1000,
                        // Longitude in [-180, 179], latitude in [-85, 84] to stay within valid
                        // 2dsphere bounds.
                        loc: {type: "Point", coordinates: [(i % 360) - 180, (i % 170) - 85]},
                    },
                },
            });
            if (ops.length === 1000 || i === numDocs - 1) {
                assert.commandWorked(geoColl.bulkWrite(ops));
                ops = [];
            }
        }
        assert.commandWorked(geoColl.createIndex({f1: 1}));
        assert.commandWorked(geoColl.createIndex({loc: "2dsphere"}));
    }

    checkRanker({
        qID: "2.2",
        cName: "geo",
        query: {loc: {$near: {$geometry: {type: "Point", coordinates: [0, 0]}}}, f1: {$gt: 100}},
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kInestimableNode,
    });

    // (3) AutomaticCE chooses CBR because of very low productivity
    checkRanker({
        qID: "3.1",
        cName: "20k",
        query: {f1: {$gte: 0}, x2: {$gte: 998}},
        chosenRanker: ChosenRanker.kCostBased,
        reason: PlanRankerReason.kCbrCheaperThanMp,
    });
    checkRanker({
        qID: "3.2",
        cName: "20k",
        query: {f1: {$lt: 505}, f2: {$gt: 100}},
        order: {x1: 1},
        limit: batchSize + 1,
        chosenRanker: ChosenRanker.kCostBased,
        reason: PlanRankerReason.kCbrCheaperThanMp,
    });
    // This is an interesting case with productivity = 0.0052 which is < 0.0101
    checkRanker({
        qID: "3.3",
        cName: "20k",
        query: {
            $expr: {
                $and: [
                    {$gt: ["$f1", 42]},
                    {$lt: ["$f4", 420]},
                    {$eq: [{$mod: [{$add: ["$f2", "$f3"]}, 2]}, 1]},
                    {$lt: [{$add: ["$f2", "$f3"]}, 100]},
                ],
            },
        },
        chosenRanker: ChosenRanker.kCostBased,
        reason: PlanRankerReason.kCbrCheaperThanMp,
    });

    // (4) AutomaticCE chooses MP because the required improvement is not achievable
    // Make this test more stable by increasing the required ratio - it works with the default but
    // sometimes the ratio may occasionally get better.
    const prevRatio = assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryMinRequiredImprovementRatioForCostBasedRankerChoice: 3.0,
        }),
    ).was;
    checkRanker({
        qID: "4.1",
        cName: "20k",
        query: {f1: {$gte: 0}, x2: {$gte: 900}},
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kMpCheaperThanCbr,
    });
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryMinRequiredImprovementRatioForCostBasedRankerChoice: prevRatio,
        }),
    );

    // (5) AutomaticCE chooses CBR because it is cheaper than MP
    checkRanker({
        qID: "5.1",
        cName: "20k",
        query: {f1: {$gte: 0}, x2: {$gte: 940}},
        chosenRanker: ChosenRanker.kCostBased,
        reason: PlanRankerReason.kCbrCheaperThanMp,
    });
    checkRanker({
        qID: "5.2",
        cName: "20k",
        query: {f1: {$lt: 505}, f2: {$gt: 100}},
        order: {f3: 1},
        limit: batchSize + 1,
        chosenRanker: ChosenRanker.kCostBased,
        reason: PlanRankerReason.kCbrCheaperThanMp,
    });

    // ----------------------------------------------------------------------------------------
    // NoMultiplanningResults: CBR is engaged only when MP produced no results within its
    // trial budget. (Scenarios ported from cbr_for_no_mp_results.js.)
    // ----------------------------------------------------------------------------------------
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            featureFlagCostBasedRanker: true,
            internalQueryPlanRanker: "mixed",
            internalQueryCBRCEMode: "samplingCE",
            internalQueryMixedPlanRankingStrategy: "NoMultiplanningResults",
        }),
    );

    // Deterministic sample generation for stable plan selection, and frequent yields to exercise
    // yield/restore during the trial phase.
    const prevSequentialSamplingScan = assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}),
    ).was;
    const prevExecYieldIterations = assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}),
    ).was;

    try {
        // 'f1 >= 0' and 'f2 >= 0' match every document (giving competing full index scans on f1 and
        // f2), but no document has field 'c', so the query returns nothing. On the 20k collection
        // neither plan can reach EOF within its trial budget, so both exhaust it without a result
        // and CBR breaks the tie.
        const noResultsQuery = {f1: {$gte: 0}, f2: {$gte: 0}, c: 1};
        checkRanker({
            qID: "6.1",
            cName: "20k",
            query: noResultsQuery,
            chosenRanker: ChosenRanker.kCostBased,
            reason: PlanRankerReason.kNoMultiplanningResults,
        });
        // The same predicates without the impossible 'c' clause match every document, so MP finds
        // results during the trial phase and picks the winner without engaging CBR.
        checkRanker({
            qID: "6.2",
            cName: "20k",
            query: {f1: {$gte: 0}, f2: {$gte: 0}},
            chosenRanker: ChosenRanker.kMultiPlanning,
            reason: PlanRankerReason.kMpEarlyExitOrResult,
        });
        // 'c' is not indexed, so there is a single candidate plan (a collection scan) and no ranking
        // is needed.
        checkRanker({
            qID: "6.3",
            cName: "20k",
            query: {c: 1},
            chosenRanker: ChosenRanker.kNone,
            reason: PlanRankerReason.kSinglePlan,
        });
        // 'f1' and 'f2' have no values this large, so both index scans are empty and MP reaches EOF
        // early, picking the winner without engaging CBR.
        checkRanker({
            qID: "6.4",
            cName: "20k",
            query: {f1: 100000, f2: 100000},
            chosenRanker: ChosenRanker.kMultiPlanning,
            reason: PlanRankerReason.kMpEarlyExitOrResult,
        });
        // No MP results, so CBR is engaged, but $returnKey makes every plan inestimable
        // (RETURN_KEY), so CBR falls back to MP.
        checkRanker({
            qID: "6.5",
            cName: "20k",
            query: noResultsQuery,
            returnKey: true,
            chosenRanker: ChosenRanker.kMultiPlanning,
            reason: PlanRankerReason.kInestimableNode,
        });
    } finally {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingBySequentialScan: prevSequentialSamplingScan,
                internalQueryExecYieldIterations: prevExecYieldIterations,
            }),
        );
    }

    // ----------------------------------------------------------------------------------------
    // Configuration-driven ranker choice: the ranker is determined by configuration rather than by
    // the cost-based decision itself.
    // ----------------------------------------------------------------------------------------

    // 'f1 >= 0' and 'f2 >= 0' give two competing full index scans.
    const configMultiPlanQuery = {f1: {$gte: 0}, f2: {$gte: 0}};

    // CBR disabled via the feature flag: the multi-planner ranks the query.
    assert.commandWorked(db.adminCommand({setParameter: 1, featureFlagCostBasedRanker: false}));
    checkRanker({
        qID: "7.1 - feature-flag-off",
        cName: "20k",
        query: configMultiPlanQuery,
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kFeatureFlag,
    });

    // A concrete CE mode forces CBR to rank every multi-plan query directly. Deterministic sample
    // generation keeps plan selection stable.
    const prevSequentialSamplingScanForKnob = assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}),
    ).was;
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            featureFlagCostBasedRanker: true,
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "samplingCE",
        }),
    );
    try {
        checkRanker({
            qID: "7.2 - query-knob",
            cName: "20k",
            query: configMultiPlanQuery,
            chosenRanker: ChosenRanker.kCostBased,
            reason: PlanRankerReason.kQueryKnob,
        });
        // CBR is forced on, but $returnKey introduces a RETURN_KEY stage that CBR cannot estimate,
        // so it falls back to the multi-planner.
        checkRanker({
            qID: "7.3 - query-knob-inestimable",
            cName: "20k",
            query: configMultiPlanQuery,
            returnKey: true,
            chosenRanker: ChosenRanker.kMultiPlanning,
            reason: PlanRankerReason.kInestimableNode,
        });
    } finally {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingBySequentialScan: prevSequentialSamplingScanForKnob,
            }),
        );
    }
} finally {
    // Restore the CBR parameters this test changed. We restore them directly (rather than via
    // setCBRConfig) to avoid touching internalSamplingSizeOverride, which this test never modifies.
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            featureFlagCostBasedRanker: prevCBRConfig.featureFlagCostBasedRanker,
            internalQueryPlanRanker: prevCBRConfig.internalQueryPlanRanker,
            internalQueryCBRCEMode: prevCBRConfig.internalQueryCBRCEMode,
            internalQueryMixedPlanRankingStrategy:
                prevCBRConfig.internalQueryMixedPlanRankingStrategy,
        }),
    );
}
