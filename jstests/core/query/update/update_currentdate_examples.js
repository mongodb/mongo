// @tags: [requires_non_retryable_writes]

// Basic examples for $currentDate
let res;
const coll = db[jsTestName()];
coll.drop();

// $currentDate default
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.save({_id: 1, a: 2}));
assert.commandWorked(coll.update({}, {$currentDate: {a: true}}));
assert(coll.findOne().a.constructor == Date);

// $currentDate type = date
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.save({_id: 1, a: 2}));
res = coll.update({}, {$currentDate: {a: {$type: "date"}}});
assert.commandWorked(res);
assert(coll.findOne().a.constructor == Date);

// $currentDate type = timestamp
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.save({_id: 1, a: 2}));
assert.commandWorked(coll.update({}, {$currentDate: {a: {$type: "timestamp"}}}));
assert(coll.findOne().a.constructor == Timestamp);
