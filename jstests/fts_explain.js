// Test $text explain.  SERVER-12037.

var coll = db.fts_explain;

coll.drop();
coll.ensureIndex({content: "text"}, {default_language: "none"});
assert.gleSuccess(db);

coll.insert({content: "some data"});
assert.gleSuccess(db);

var explain = coll.find({$text:{$search: "\"a\" -b -\"c\""}}).explain(true);
assert.eq(explain.cursor, "TextCursor");
assert.eq(explain.stats.type, "TEXT");
assert.eq(explain.stats.parsedTextQuery.terms, ["a"]);
assert.eq(explain.stats.parsedTextQuery.negatedTerms, ["b"]);
assert.eq(explain.stats.parsedTextQuery.phrases, ["a"]);
assert.eq(explain.stats.parsedTextQuery.negatedPhrases, ["c"]);
