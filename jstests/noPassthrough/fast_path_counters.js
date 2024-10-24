/**
 * Tests that query planning fast path counters are updated correctly.
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    getWinningPlanFromExplain,
    isExpress,
    isIdhack,
    isIdhackOrExpress
} from "jstests/libs/query/analyze_plan.js";

const collName = jsTestName();
const conn = MongoRunner.runMongod({});
const db = conn.getDB(jsTestName());
const collection = assertDropAndRecreateCollection(db, collName);

function assertIdHackCounterIncreased(fn) {
    const prevFastPathPlanningMetrics = db.serverStatus().metrics.query.planning.fastPath;
    fn();
    const fastPathPlanningMetrics = db.serverStatus().metrics.query.planning.fastPath;
    assert.eq(prevFastPathPlanningMetrics.idHack + 1, fastPathPlanningMetrics.idHack);
    assert.eq(prevFastPathPlanningMetrics.express, fastPathPlanningMetrics.express);
}

function assertNoFastPathCounterIncreased(fn) {
    const prevFastPathPlanningMetrics = db.serverStatus().metrics.query.planning.fastPath;
    fn();
    const fastPathPlanningMetrics = db.serverStatus().metrics.query.planning.fastPath;
    assert.eq(prevFastPathPlanningMetrics.idHack, fastPathPlanningMetrics.idHack);
    assert.eq(prevFastPathPlanningMetrics.express, fastPathPlanningMetrics.express);
}

function assertExpressCounterIncreased(fn) {
    const prevFastPathPlanningMetrics = db.serverStatus().metrics.query.planning.fastPath;
    fn();
    const fastPathPlanningMetrics = db.serverStatus().metrics.query.planning.fastPath;
    assert.eq(prevFastPathPlanningMetrics.idHack, fastPathPlanningMetrics.idHack);
    assert.eq(prevFastPathPlanningMetrics.express + 1, fastPathPlanningMetrics.express);
}

// Tests that a query not using idHack or express does not increment fast path counters.
assertNoFastPathCounterIncreased(() => {
    // idHack queries cannot be used with skip().
    jsTestLog("Testing with no idHack query");
    const explain = collection.find({_id: {a: 1}}).skip(1).explain();
    const winningPlan = getWinningPlanFromExplain(explain);
    assert(!isIdhackOrExpress(db, winningPlan), winningPlan);
});

// Tests that a find query using idHack, increments the idHack counter.
assertIdHackCounterIncreased(() => {
    jsTestLog("Testing with idHack find query");
    // Needs batchSize, otherwise this query would be resolved with express instead.
    const explain = collection.find({_id: {a: 1}}).batchSize(10).explain();
    const winningPlan = getWinningPlanFromExplain(explain);
    assert(isIdhack(db, winningPlan), winningPlan);
});

// Tests that an update query using express path, increments the express counter.
assertExpressCounterIncreased(() => {
    jsTestLog("Testing with express update query");
    const explain = db.runCommand(
        {explain: {update: collName, updates: [{q: {_id: {a: 1}}, u: {$set: {a: 2}}}]}});
    const winningPlan = getWinningPlanFromExplain(explain);
    assert(isExpress(db, winningPlan), winningPlan);
});

// Tests that a delete query using express increments the express counter.
assertExpressCounterIncreased(() => {
    jsTestLog("Testing with express delete query");
    const explain =
        db.runCommand({explain: {delete: collName, deletes: [{q: {_id: {a: 1}}, limit: 0}]}});
    const winningPlan = getWinningPlanFromExplain(explain);
    assert(isExpress(db, winningPlan), winningPlan);
});

// Tests that a find query using express, increments the express counter.
assertExpressCounterIncreased(() => {
    jsTestLog("Testing with express find query");
    const explain = collection.find({_id: 1}).explain();
    const winningPlan = getWinningPlanFromExplain(explain);
    assert(isExpress(db, winningPlan), winningPlan);
});

MongoRunner.stopMongod(conn);
