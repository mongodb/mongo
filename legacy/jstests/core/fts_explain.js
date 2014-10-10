// Test $text explain.  SERVER-12037.

var coll = db.fts_explain;
var res;

coll.drop();
res = coll.ensureIndex({content: "text"}, {default_language: "none"});
assert.commandWorked(res);

res = coll.insert({content: "some data"});
assert.writeOK(res);

var explain = coll.find({$text:{$search: "\"a\" -b -\"c\""}}).explain(true);
var stage = explain.executionStats.executionStages;
assert.eq(stage.stage, "TEXT");
assert.eq(stage.parsedTextQuery.terms, ["a"]);
assert.eq(stage.parsedTextQuery.negatedTerms, ["b"]);
assert.eq(stage.parsedTextQuery.phrases, ["a"]);
assert.eq(stage.parsedTextQuery.negatedPhrases, ["c"]);
