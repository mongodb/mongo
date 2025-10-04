// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//   assumes_no_implicit_index_creation,
// ]

// Test $text explain.  SERVER-12037.

const coll = db.fts_explain;
let res;

coll.drop();
res = coll.createIndex({content: "text"}, {default_language: "none"});
assert.commandWorked(res);

res = coll.insert({content: "some data"});
assert.commandWorked(res);

const explain = coll.find({$text: {$search: '"a" -b -"c"'}}, {content: 1, score: {$meta: "textScore"}}).explain(true);
let stage = explain.executionStats.executionStages;
if ("SINGLE_SHARD" === stage.stage) {
    stage = stage.shards[0].executionStages;
}

assert.eq(stage.stage, "PROJECTION_DEFAULT");

let textStage = stage.inputStage;
assert.eq(textStage.stage, "TEXT_MATCH");
assert.gte(textStage.textIndexVersion, 1, "textIndexVersion incorrect or missing.");
assert.eq(textStage.inputStage.stage, "TEXT_OR");
assert.eq(textStage.parsedTextQuery.terms, ["a"]);
assert.eq(textStage.parsedTextQuery.negatedTerms, ["b"]);
assert.eq(textStage.parsedTextQuery.phrases, ["a"]);
assert.eq(textStage.parsedTextQuery.negatedPhrases, ["c"]);
