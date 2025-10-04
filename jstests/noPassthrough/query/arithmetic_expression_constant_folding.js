/**
 * Randomized testing to confirm the correctness of left-to-right associativity for arithmetic
 * operations that take multiple arguments.
 */

// Randomized property testing.
let conn = MongoRunner.runMongod();
const dbName = jsTestName();
const testDB = conn.getDB(dbName);
const coll = testDB.getCollection("coll");
coll.drop();

function assertPipelineCorrect(pipeline, v) {
    coll.drop();
    coll.insert({v});
    testDB.adminCommand({
        configureFailPoint: "disablePipelineOptimization",
        mode: "off",
    });
    let optimizedResults = coll.aggregate(pipeline).toArray();
    testDB.adminCommand({
        configureFailPoint: "disablePipelineOptimization",
        mode: "alwaysOn",
    });
    let unoptimizedResults = coll.aggregate(pipeline).toArray();
    testDB.adminCommand({
        configureFailPoint: "disablePipelineOptimization",
        mode: "off",
    });
    assert.eq(unoptimizedResults.length, 1);
    assert.eq(optimizedResults.length, 1);

    // It is possible for optimization to cause the floating point results to differ slightly due to
    // precision limitations, so we must only assert an equality violation when the delta is not
    // infinitesimal. We have observed values differing by one in the 16th significant digit.
    if (Math.abs(unoptimizedResults[0]._id - optimizedResults[0]._id) > 1e-14) {
        // This will now fail, but we know the difference is more than 1e-14 so we actually care.
        assert.eq(unoptimizedResults[0]._id, optimizedResults[0]._id, tojson(pipeline));
    }
}

/**
 * Randomized, property-based test of the left-to-right constant folding optimization. The purpose
 * of folding left-to-right is to preserve the same order-of-operations during ahead-of-time
 * constant folding that occurs during runtime execution.
 *
 * Given:
 *  - A random list of numbers of any type
 *  - A fieldpath reference placed at a random location in the list of numbers
 *  - A pipeline that performs an arithmetic operation over the list of arguments (fieldpath +
 *      numbers)
 * Show:
 *  - The arithmetic operation produces the exact same result with and without optimizations.
 * @param {options} options
 */
function runRandomizedPropertyTest({op, min, max}) {
    // Function to generate random numbers of float, long, double, and NumberDecimal (with different
    // probabilities).
    const generateNumber = () => {
        const r = Math.random() * (max - min) + min;
        const t = Math.random();
        if (t < 0.7) {
            return r;
        }
        if (t < 0.85) {
            return NumberInt(Math.round(r));
        }
        if (t < 0.99) {
            return NumberLong(Math.round(r));
        }
        return NumberDecimal(String(r));
    };

    const generateNumberList = (length) => Array.from({length}, () => generateNumber(min, max));

    const numbers = generateNumberList(10);
    // Place a fieldpath reference randomly within the list of numbers to produce an argument list.
    const pos = Math.floor(numbers.length * Math.random());
    const args = [].concat(numbers.slice(0, pos), ["$v"], numbers.slice(pos));

    const pipeline = [
        {
            $group: {
                _id: {[op]: args},
                sum: {$sum: 1},
            },
        },
    ];
    coll.drop();
    const v = generateNumber();
    assertPipelineCorrect(pipeline, v);
}

for (let i = 0; i < 5; i++) {
    runRandomizedPropertyTest({op: "$add", min: -314159255, max: 314159255});
    runRandomizedPropertyTest({op: "$multiply", min: -31415, max: 31415});
}
MongoRunner.stopMongod(conn);
