import {getWinningPlanFromExplain, getExecutionStats} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted, assertPlanNotCosted, getMultiplanningBatchSize} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {LcgRandom} from "jstests/libs/lcg_random.js";

// TODO SERVER-92589: Remove this exemption
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
];

const nFields = Math.max(...compoundIndexes.map((obj) => Object.keys(obj).length));
const batchSize = getMultiplanningBatchSize();
const mpEndConditions = {kEOF: "EOF", kFullBatch: "FullBatch", kMaxWorks: "MaxWorks"};
const rankerStrategies = {kCBR: "CBR", kMP: "MP"};

function checkRanker({qID = "", cName = "", query = {}, order = {}, limit = 0, mpEndCond = "", ranker = ""}) {
    const coll = db[collName(cName)];

    let cursor = coll.find(query).sort(order);
    if (limit > 0) {
        cursor = cursor.limit(limit);
    }
    jsTest.log.info(`Testing case: ${qID}`);
    const explain = assert.commandWorked(cursor.explain("allPlansExecution"));
    const winningPlan = getWinningPlanFromExplain(explain);

    switch (mpEndCond) {
        // TODO SERVER-115896 use explain to extract and check the corresponding properties
        case mpEndConditions.kEOF: {
            break;
        }
        case mpEndConditions.kFullBatch: {
            break;
        }
        case mpEndConditions.kMaxWorks: {
            break;
        }
        default: {
            throw new Error(`Invalid expected ${mpEndCond}.`);
        }
    }

    switch (ranker) {
        case rankerStrategies.kCBR: {
            assertPlanCosted(winningPlan);
            break;
        }
        case rankerStrategies.kMP: {
            assertPlanNotCosted(winningPlan);
            break;
        }
        default: {
            throw new Error(`Invalid expected ranker ${ranker}.`);
        }
    }
}

populateCollection("100", 100, nFields, compoundIndexes);
populateCollection("20k", 20000, nFields, compoundIndexes);

const prevPlanRankerMode = assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"})).was;
const prevAutoPlanRankingStrategy = assert.commandWorked(
    db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: "CBRCostBasedRankerChoice"}),
).was;
try {
    // The implementation of AutomaticCE with a cost-based choice of the plan ranker
    // considers 4 different cases. Each of these cases is listed below and tested.

    // (1) AutomaticCE chooses MP because of EOF or full batch
    // 1.1 EOF small collection
    checkRanker({
        qID: "1.1.1",
        cName: "100",
        query: {f1: {$gte: 0}, f2: {$lte: 0}},
        mpEndCond: mpEndConditions.kEOF,
        ranker: rankerStrategies.kMP,
    });
    checkRanker({
        qID: "1.1.2",
        cName: "100",
        query: {f1: {$gte: 0}, f2: {$lte: 0}},
        order: {f1: 1, x1: 1},
        mpEndCond: mpEndConditions.kEOF,
        ranker: rankerStrategies.kMP,
    });
    checkRanker({
        qID: "1.1.3",
        cName: "100",
        query: {
            $and: [
                {$or: [{f1: {$gte: 100}}, {f2: {$lte: 901}}, {f3: {$gte: 50}}, {f4: {$lte: 990}}, {f5: {$gte: 10}}]},
                {$or: [{f1: {$lte: 150}}, {f2: {$gte: 910}}, {f3: {$lte: 250}}, {f4: {$gte: 870}}, {f5: {$lte: 25}}]},
            ],
        },
        order: {f3: 1, f1: 1},
        mpEndCond: mpEndConditions.kEOF,
        ranker: rankerStrategies.kMP,
    });
    // 1.2 EOF big collection
    checkRanker({
        qID: "1.2.1",
        cName: "20k",
        query: {f1: 500, f2: {$gt: 300}},
        mpEndCond: mpEndConditions.kEOF,
        ranker: rankerStrategies.kMP,
    });
    checkRanker({
        qID: "1.2.2",
        cName: "20k",
        query: {f1: {$in: [500, 600]}, f2: {$gt: 100}},
        order: {f1: 1},
        limit: batchSize,
        mpEndCond: mpEndConditions.kEOF,
        ranker: rankerStrategies.kMP,
    });

    // 1.3 full batch
    checkRanker({
        qID: "1.3.1",
        cName: "20k",
        query: {f1: {$lt: 505}, f2: {$gt: 990}},
        mpEndCond: mpEndConditions.kFullBatch,
        ranker: rankerStrategies.kMP,
    });
    checkRanker({
        qID: "1.3.2",
        cName: "20k",
        query: {f1: {$lt: 505}, f2: {$lt: 200}},
        order: {f3: 1},
        limit: batchSize + 1,
        mpEndCond: mpEndConditions.kFullBatch,
        ranker: rankerStrategies.kMP,
    });

    // (2) AutomaticCE chooses CBR because of very low productivity
    checkRanker({
        qID: "2.1",
        cName: "20k",
        query: {f1: {$gte: 0}, x2: {$gte: 998}},
        mpEndCond: mpEndConditions.kMaxWorks,
        ranker: rankerStrategies.kCBR,
    });
    checkRanker({
        qID: "2.2",
        cName: "20k",
        query: {f1: {$lt: 505}, f2: {$gt: 100}},
        order: {x1: 1},
        limit: batchSize + 1,
        mpEndCond: mpEndConditions.kMaxWorks,
        ranker: rankerStrategies.kCBR,
    });
    // This is an interesting case with productivity = 0.0052 which is < 0.0101
    checkRanker({
        qID: "2.3",
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
        mpEndCond: mpEndConditions.kMaxWorks,
        ranker: rankerStrategies.kCBR,
    });

    // (3) AutomaticCE chooses MP because the required improvement is not achievable
    // Make this test more stable by increasing the required ratio - it works with the default but
    // sometimes the ratio may occasionally get better.
    const prevRatio = assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMinRequiredImprovementRatioForCostBasedRankerChoice: 3.0}),
    ).was;
    checkRanker({
        qID: "3.1",
        cName: "20k",
        query: {f1: {$gte: 0}, x2: {$gte: 900}},
        mpEndCond: mpEndConditions.kMaxWorks,
        ranker: rankerStrategies.kMP,
    });
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMinRequiredImprovementRatioForCostBasedRankerChoice: prevRatio}),
    );

    // (4) AutomaticCE chooses CBR because it is cheaper than MP
    checkRanker({
        qID: "4.1",
        cName: "20k",
        query: {f1: {$gte: 0}, x2: {$gte: 940}},
        mpEndCond: mpEndConditions.kMaxWorks,
        ranker: rankerStrategies.kCBR,
    });
    checkRanker({
        qID: "4.2",
        cName: "20k",
        query: {f1: {$lt: 505}, f2: {$gt: 100}},
        order: {f3: 1},
        limit: batchSize + 1,
        mpEndCond: mpEndConditions.kFullBatch,
        ranker: rankerStrategies.kCBR,
    });
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: prevPlanRankerMode}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: prevAutoPlanRankingStrategy}),
    );
}
