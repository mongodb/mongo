/**
 * Tests the behavior of the internalQueryPlanTotalEvaluationCollFraction parameter.
 */

const dbName = jsTestName();
const collName = jsTestName();

const conn = MongoRunner.runMongod({});
const db = conn.getDB(dbName);

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryDisablePlanCache: true}));
// To avoid needing a huge collection to see the effects of the collFraction limit kick in.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 10}));

const coll = db.getCollection(collName);
coll.drop();

const numDocs = 10000;
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

function getOptimizationTimeMillis(collFraction) {
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryPlanTotalEvaluationCollFraction: collFraction}));

    const explain = coll.find(query).explain();
    return explain.queryPlanner.optimizationTimeMillis;
}

// Set a really low limit so that we get a guaranteed difference even with a small collection.
const optimizationTimeWithLimit = getOptimizationTimeMillis(0.2);
// Pick some arbitrarily high ceiling that the multiplanner will never hit.
const optimizationTimeWithoutLimit = getOptimizationTimeMillis(1000.0);

// Optimization time with limit is around 5-10x faster in reality. We relax that assertion to 2x to
// reduce flakiness.
assert.gt(optimizationTimeWithoutLimit, optimizationTimeWithLimit * 2);

MongoRunner.stopMongod(conn);
