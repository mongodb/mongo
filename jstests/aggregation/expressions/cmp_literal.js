const collName = jsTestName();
const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insertOne({string: "foo"}));

// check that without $literal we end up comparing a field with itself and the result is true
var result = db.runCommand({
    aggregate: collName,
    pipeline: [{$project: {stringis$string: {$eq: ["$string", '$string']}}}],
    cursor: {}
});
assert.eq(result.cursor.firstBatch[0].stringis$string, true);

// check that with $literal we end up comparing a field with '$string' and the result is true
result = db.runCommand({
    aggregate: collName,
    pipeline: [{$project: {stringis$string: {$eq: ["$string", {$literal: '$string'}]}}}],
    cursor: {}
});
assert.eq(result.cursor.firstBatch[0].stringis$string, false);
