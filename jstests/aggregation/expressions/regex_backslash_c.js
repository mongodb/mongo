/**
 * Tests that regex matches using \C (single-byte match) that cross UTF-8 character boundaries
 * are rejected with a uassert.
 */
const coll = db.regex_backslash_c;
coll.drop();
assert.commandWorked(coll.insertOne({_id: 0, s: "é"}));
assert.commandWorked(coll.insertOne({_id: 1, s: "abc"}));

assert.throwsWithCode(
    () => coll.aggregate([{$project: {o: {$regexFindAll: {input: "$s", regex: "\\C\\K\\C"}}}}]).toArray(),
    [12407700],
);
assert.throwsWithCode(
    () => coll.aggregate([{$project: {o: {$regexFind: {input: "$s", regex: "\\C"}}}}]).toArray(),
    [12407700],
);
assert.throwsWithCode(
    () => coll.aggregate([{$project: {o: {$regexMatch: {input: "$s", regex: "\\C"}}}}]).toArray(),
    [12407700],
);
assert.throwsWithCode(() => coll.find({s: {$regex: "\\C"}}).toArray(), [12407700]);
