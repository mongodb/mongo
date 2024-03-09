// Tests that numReads field in SBE explain is roughly equal to totalDocsExamined +
// totalKeysExamined in classic explain. For find queries for SBE using the classic multiplanner, we
// set the numReads fields as totalDocsExamined + totalKeysExamined in the SBE plan cache. This is
// used when replanning the find part of the query using the SBE planner, so we expect them to be
// roughly equal to ensure we are using the correct statistic.

import {getAllPlanStages} from "jstests/libs/analyze_plan.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

function runExplain(coll, filter, proj, sort) {
    const isProjEmpty = Object.keys(proj).length === 0;
    const isSortEmpty = Object.keys(sort).length === 0;
    if (isProjEmpty && isSortEmpty) {
        return assert.commandWorked(coll.find(filter).explain("executionStats"));
    } else if (isProjEmpty) {
        return assert.commandWorked(coll.find(filter).sort(sort).explain("executionStats"));
    } else if (isSortEmpty) {
        return assert.commandWorked(coll.find(filter, proj).explain("executionStats"));
    }
    return assert.commandWorked(coll.find(filter, proj).sort(sort).explain("executionStats"));
}

function calculateNumReads(execStages) {
    const allStages = getAllPlanStages(execStages);
    let numReads = 0;
    for (let stage of allStages) {
        if (stage.hasOwnProperty("numReads")) {
            numReads += NumberInt(stage.numReads);
        }
    }
    return numReads;
}

function getTotalDocsExaminedAndTotalKeysExaminedSum(executionStats) {
    return NumberInt(executionStats.totalDocsExamined) +
        NumberInt(executionStats.totalKeysExamined);
}

/**
 * Asserts that the difference between 'lhs' and 'rhs' is at most 1.
 */
function assertAlmostEqual(lhs, rhs) {
    assert.lte(Math.abs(lhs - rhs),
               1,
               "Classic stats totalDocsExamined + totalKeysExamined (=" + lhs +
                   ") and SBE stat numReads (=" + rhs + ") are not approximately equal");
}

/**
 * Runs explain with executionStats on a find query with filter using the classic and SBE engine,
 * and then checks if numReads field in SBE explain is roughly equal to totalDocsExamined +
 * totalKeysExamined in classic explain.
 */
function checkStatsAreCorrect(db, coll, filter, proj = {}, sort = {}) {
    let classicExplain;
    let sbeEngineExplain;

    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
    classicExplain = runExplain(coll, filter, proj, sort);
    assert(classicExplain.hasOwnProperty("executionStats"), classicExplain);

    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));
    sbeEngineExplain = runExplain(coll, filter, proj, sort);
    assert(sbeEngineExplain.hasOwnProperty("executionStats"), sbeEngineExplain);

    assertAlmostEqual(getTotalDocsExaminedAndTotalKeysExaminedSum(classicExplain.executionStats),
                      calculateNumReads(sbeEngineExplain.executionStats.executionStages));
}

function test(db) {
    const coll = assertDropAndRecreateCollection(db, jsTestName());

    const docs = [];
    const numDocs = 1000;
    for (let i = 0; i < numDocs; i++) {
        docs.push({_id: i, a: i, b: i, c: i});
    }

    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.commandWorked(coll.insertMany(docs));

    // COLLSCAN.
    checkStatsAreCorrect(db, coll, {b: {$gt: 0, $lt: 900}, c: {$gt: 0, $lt: 900}});

    // IXSCAN + FETCH.
    checkStatsAreCorrect(db, coll, {a: {$gt: 0, $lt: 900}, b: {$gt: 0, $lt: 900}});

    // IXSCAN + PROJECTION_COVERED. One match.
    checkStatsAreCorrect(db, coll, {a: 30, b: 30}, {a: 1, _id: 0});

    // IXSCAN + PROJECTION_COVERED. Multiple matches.
    checkStatsAreCorrect(db, coll, {a: {$lte: 25}, b: {$lte: 30}}, {a: 1, _id: 0});

    // IXSCAN + FETCH + SORT.
    checkStatsAreCorrect(db, coll, {a: {$gt: 0, $lt: 900}, b: {$gt: 0, $lt: 900}}, {}, {c: 1});

    // IXSCAN + FETCH with high number of seeks. 'b' is out of bounds, forcing many seeks.
    checkStatsAreCorrect(db, coll, {a: {$gt: 0, $lt: 900}, b: {$gt: 1000, $lt: 1900}});

    // IXSCAN + FETCH with large $in array.
    // We do not create a continuous array for $in, since in the classic engine, we do one less seek
    // if the next document fits in an index bounds, whereas in SBE, we would do additional seeks.
    const inArray = [];
    for (let i = 0; i < 400; i++) {
        if (i % 2 == 0) {
            inArray.push(i);
        }
    }
    checkStatsAreCorrect(db, coll, {a: {$in: inArray}});
}

{
    const conn = MongoRunner.runMongod();
    test(conn.getDB('MongoD'));
    MongoRunner.stopMongod(conn);
}

{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    test(rst.getPrimary().getDB("ReplSetTestDB"));
    rst.stopSet();
}
