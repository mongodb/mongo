/**
 * @tags: [
 *   # TODO (SERVER-116395): Re-enable this test with primary-driven index builds.
 *   primary_driven_index_builds_incompatible_with_retryable_writes,
 * ]
 */
const c = db[jsTestName()];

c.drop();

assert.commandWorked(c.insert({_id: 5, a: {b: 1}}));
let ret = c.findAndModify({
    query: {"a.b": 1},
    update: {$set: {"a.b": 2}}, // Ensure the query on 'a.b' no longer matches.
    new: true,
});
assert.eq(5, ret._id);
assert.eq(2, ret.a.b);

assert(c.drop());

assert.commandWorked(c.insert({_id: null, a: {b: 1}}));
ret = c.findAndModify({
    query: {"a.b": 1},
    update: {$set: {"a.b": 2}}, // Ensure the query on 'a.b' no longer matches.
    new: true,
});
assert.eq(2, ret.a.b);
