/**
 * Reproduces SERVER-105903.
 */

load("jstests/libs/analyze_plan.js");

const getParam = db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1});
const sbeDisabledOrRestricted = getParam.hasOwnProperty("internalQueryFrameworkControl") &&
    ["forceClassicEngine", "trySbeRestricted"].includes(
        getParam.internalQueryFrameworkControl.value);
if (!sbeDisabledOrRestricted) {
    // The wrong behavior being tested here is fixed by disabling parameterization when SBE is
    // disabled. Hence the test will still fail with SBE enabled.
    jsTestLog("SBE is enabled, skipping the test");
    quit();
}

const coll = db[jsTestName()];
coll.drop();

const docs = [
    {_id: 1},
];
assert.commandWorked(coll.insert(docs));

assert.commandWorked(coll.createIndex({
    a: 1,
    b: 1,
}));

const explain = coll.find({
                        "$and": [
                            {a: 1},
                            {a: 1},
                        ]
                    })
                    .sort({b: 1})
                    .explain();

jsTestLog("Explain output: " + tojson(explain));

const winningPlan = getWinningPlanFromExplain(explain);
const hasSort = planHasStage(db, winningPlan, "SORT");

// We expect that we can use a backward scan of the index to satisfy the sort without needing a
// blocking SORT stage.
assert(!hasSort, "Expected no SORT stage in the winning plan, but found one.");
