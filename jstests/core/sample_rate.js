/**
 * Test the $sampleRate match expression.
 * @tags: [
 *     # TODO(SERVER-60823): remove incompatible_with_gcov
 *     incompatible_with_gcov,
 *     # TODO SERVER-87186: aggregation can fail with QueryPlanKilled in suites with random
 *     # migrations because moveCollection change the collection UUID
 *     # by dropping and re-creating the collection
 *     assumes_balancer_off,
 * ]
 */
const coll = db.expression_sample_rate;
coll.drop();

print("Generating test collection...");
const N = 3000;
let i;
const bulk = coll.initializeUnorderedBulkOp();
for (i = 0; i < N; i++) {
    bulk.insert({_id: i, v: 1});
}
assert.commandWorked(bulk.execute());

const p = 0.5;
const k = 1000;

// Average the number of docs sampled over k iterations.
const pipeline = [{$match: {$sampleRate: p}}, {$count: "n"}];
let nSampled = 0;
for (i = 0; i < k; i++) {
    const resultArray = coll.aggregate(pipeline).toArray();
    assert.eq(1, resultArray.length);
    nSampled += resultArray[0]["n"];
}
const avg = nSampled / k;
print("Average docs sampled: ", avg);

// Test that the average number of sampled docs is within 10 standard deviations using the
// binomial distribution over k runs, 10 * sqrt(N * p * (1 - p) / k).
const mu = p * N;
const err = 10.0 * Math.sqrt(mu * (1 - p) / k);
assert.between(mu - err, avg, mu + err);

// Test that we accept 0.0 and 1.0.
let resultArray = coll.aggregate([{$match: {$sampleRate: 0.0}}]).toArray();
assert.eq(0, resultArray.length);

resultArray = coll.aggregate([{$match: {$sampleRate: 0}}]).toArray();
assert.eq(0, resultArray.length);

resultArray = coll.aggregate([{$match: {$sampleRate: 1.0}}, {$count: "n"}]).toArray();
assert.eq(1, resultArray.length);
assert.eq(resultArray[0]["n"], N);

resultArray = coll.aggregate([{$match: {$sampleRate: 1}}, {$count: "n"}]).toArray();
assert.eq(1, resultArray.length);
assert.eq(resultArray[0]["n"], N);

// Test parser failure cases.
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), cursor: {}, pipeline: [{$match: {$sampleRate: -1}}]}),
    ErrorCodes.BadValue);

assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: coll.getName(), cursor: {}, pipeline: [{$match: {$sampleRate: -1.0}}]}),
    ErrorCodes.BadValue);

assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: coll.getName(), cursor: {}, pipeline: [{$match: {$sampleRate: 2.0}}]}),
    ErrorCodes.BadValue);

assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    cursor: {},
    pipeline: [{$match: {$sampleRate: {$const: 0.25}}}]
}),
                             ErrorCodes.BadValue);
