/**
 * Tests the behavior of the internalQueryPlanTotalEvaluationCollFraction parameter.
 */

const dbName = jsTestName();
const collName = jsTestName();

const conn = MongoRunner.runMongod({});
const db = conn.getDB(dbName);

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryDisablePlanCache: true}));
// To avoid needing a huge collection to see the effects of the collFraction limit kick in.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 1}));
// Ensure total coll fraction is always used in this test instead of the per-plan limit.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryPlanEvaluationCollFraction: 1}));

const coll = db.getCollection(collName);
coll.drop();

const numDocs = 10;
const bulkOp = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulkOp.insert({
        _id: i,
        nonSelective1: i,
        nonSelective2: i,
        nonSelective3: i,
        nonSelective4: i,
        nonSelective5: i,
        nonSelective6: i
    });
}
assert.commandWorked(bulkOp.execute());
assert(coll.count() == numDocs);

function generateCombinations(n) {
    let combinations = [[]];
    for (let i = 0; i < n; i++) {
        const newCombinations = [];
        for (const combo of combinations) {
            newCombinations.push([...combo, 1]);
            newCombinations.push([...combo, -1]);
        }
        combinations = newCombinations;
    }
    return combinations;
}

function generateIndexes(n) {
    const nonSelectiveFields = [
        "nonSelective1",
        "nonSelective2",
        "nonSelective3",
        "nonSelective4",
        "nonSelective5",
        "nonSelective6"
    ];
    const selectiveField = "selective";

    const directionCombinations = generateCombinations(nonSelectiveFields.length).slice(0, n);
    return directionCombinations.map((directions, index) => {
        const indexSpec = {};

        nonSelectiveFields.forEach((field, fieldIndex) => {
            indexSpec[field] = directions[fieldIndex];
        });
        indexSpec[selectiveField] = 1;
        print(`Created index ${index + 1}: ${tojson(indexSpec)}`);
        return indexSpec;
    });
}

// Create enough indexes to exercise the collFraction limit.
const indexes = generateIndexes(15);
assert.commandWorked(coll.createIndexes(indexes));

const query = {
    nonSelective1: {$gte: 0},
    nonSelective2: {$gte: 0},
    nonSelective3: {$gte: 0},
    nonSelective4: {$gte: 0},
    nonSelective5: {$gte: 0},
    nonSelective6: {$gte: 0},
    selective: {$lt: 0}
};

// Reports which multi-planner stopping condition metric increased after running a query
// with the given 'collFraction' setting.
function getStoppingCondition(collFraction) {
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryPlanTotalEvaluationCollFraction: collFraction}));

    const before = db.serverStatus().metrics.query.multiPlanner.stoppingCondition;
    coll.find(query).explain();
    const after = db.serverStatus().metrics.query.multiPlanner.stoppingCondition;
    return {
        hitEof: after.hitEof - before.hitEof,
        hitResultsLimit: after.hitResultsLimit - before.hitResultsLimit,
        hitWorksLimit: after.hitWorksLimit - before.hitWorksLimit,
    };
}

// We hit the works limit with a low total coll fraction.
assert.docEq(getStoppingCondition(0.1), {
    hitEof: 0,
    hitResultsLimit: 0,
    hitWorksLimit: 1,
});

// With a higher coll fraction, multiplanning continues until the first plan
// scans the whole index.
assert.docEq(getStoppingCondition(20.0), {
    hitEof: 1,
    hitResultsLimit: 0,
    hitWorksLimit: 0,
});

MongoRunner.stopMongod(conn);
