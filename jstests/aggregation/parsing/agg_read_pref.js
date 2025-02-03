// SERVER-7768 aggregate cmd shouldn't fail when $readPreference is specified.
//
// @tags: [
//   # This test sets a read preference itself and does not expect it to be overridden.
//   assumes_read_preference_unchanged,
// ]
const collection = db[jsTestName()];
collection.drop();
assert.commandWorked(collection.insert({foo: 1}));
// Can't use aggregate helper here because we need to add $readPreference flag.
let res = db.runCommand({
    aggregate: collection.getName(),
    pipeline: [{$project: {_id: false, foo: true}}],
    $readPreference: {mode: 'primary'},
    cursor: {}
});

assert.commandWorked(res);
assert.eq(res.cursor.firstBatch, [{foo: 1}]);