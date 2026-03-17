/**
 * Tests the serverStatus metric for the classic subplanner: query.subPlanner.classicChoseWinningPlan.
 *
 * The classic subplanner is used to plan rooted $or queries. This metric is incremented each time
 * the subplanner successfully chooses a winning plan.
 */

import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";

const collName = jsTestName();
const dbName = jsTestName();

const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB(dbName);
let coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insert({_id: 1, a: 1, b: 1}));
assert.commandWorked(coll.insert({_id: 2, a: 1, b: 1}));
assert.commandWorked(coll.insert({_id: 3, a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// Force classic engine since the metric only tracks classic subplanner invocations.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));

function getSubPlannerMetrics() {
    return db.serverStatus().metrics.query.subPlanner;
}

// Verify the metric starts at zero.
{
    const metrics = getSubPlannerMetrics();
    assert.eq(metrics.classicChoseWinningPlan, 0, "classicChoseWinningPlan should start at 0");
}

// Run a rooted $or query to trigger the subplanner.
{
    assert.commandWorked(coll.find({$or: [{a: 1}, {b: 1}]}).explain());

    const metrics = getSubPlannerMetrics();
    assert.eq(
        metrics.classicChoseWinningPlan,
        1,
        "classicChoseWinningPlan should be 1 after first subplanner invocation",
    );
}

// Run a second rooted $or query to verify accumulation.
{
    assert.commandWorked(coll.find({$or: [{a: 2}, {b: 2}]}).explain());

    const metrics = getSubPlannerMetrics();
    assert.eq(
        metrics.classicChoseWinningPlan,
        2,
        "classicChoseWinningPlan should be 2 after second subplanner invocation",
    );
}

// Verify FTDC includes subplanner metrics.
assert.soon(
    () => {
        const subPlannerFtdc = verifyGetDiagnosticData(conn.getDB("admin")).serverStatus.metrics.query.subPlanner;

        if (subPlannerFtdc.classicChoseWinningPlan != 2) {
            return false;
        }
        return true;
    },
    () =>
        "FTDC output should eventually reflect observed serverStatus metrics. Current FTDC: " +
        tojson(verifyGetDiagnosticData(conn.getDB("admin")).serverStatus.metrics.query.subPlanner),
);

MongoRunner.stopMongod(conn);
