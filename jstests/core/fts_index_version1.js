// Test basic usage of "textIndexVersion:1" indexes.
var coll = db.fts_index_version1;

// Test basic English search.
coll.drop();
assert.commandWorked(coll.ensureIndex({a: "text"}, {textIndexVersion: 1}));
assert.writeOK(coll.insert({a: "running"}));
assert.eq(1, coll.count({$text: {$search: "run"}}));

// Test search with a "language alias" only recognized in textIndexVersion:1 (note that the stopword
// machinery doesn't recognize these aliases).
coll.drop();
assert.commandWorked(coll.ensureIndex({a: "text"}, {default_language: "eng", textIndexVersion: 1}));
assert.writeOK(coll.insert({a: "running"}));
assert.eq(1, coll.count({$text: {$search: "run"}}));

// Test that textIndexVersion:1 indexes ignore subdocument language annotations.
coll.drop();
assert.commandWorked(coll.ensureIndex({"a.b": "text"}, {textIndexVersion: 1}));
assert.writeOK(coll.insert({language: "none", a: {language: "english", b: "the"}}));
assert.eq(1, coll.count({$text: {$search: "the", $language: "none"}}));
