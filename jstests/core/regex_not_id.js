// don't allow regex as _id: SERVER-9502

var testColl = db.regex_not_id;
testColl.drop();

assert.writeOK(testColl.insert({_id: "ABCDEF1"}));

// Should be an error.
assert.writeError(testColl.insert({_id: /^A/}));

// _id doesn't have to be first; still disallowed
assert.writeError(testColl.insert({xxx: "ABCDEF", _id: /ABCDEF/}));
