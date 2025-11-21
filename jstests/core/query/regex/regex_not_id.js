// @tags: [
//   # Time-series collections have different _id properties.
//   exclude_from_timeseries_crud_passthrough,
// ]

// don't allow regex as _id: SERVER-9502

let testColl = db.regex_not_id;
testColl.drop();

assert.commandWorked(testColl.insert({_id: "ABCDEF1"}));

// Should be an error.
assert.writeError(testColl.insert({_id: /^A/}));

// _id doesn't have to be first; still disallowed
assert.writeError(testColl.insert({xxx: "ABCDEF", _id: /ABCDEF/}));
