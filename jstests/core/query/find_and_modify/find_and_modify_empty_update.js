// @tags: [requires_non_retryable_writes]

// Test passing update:{} to findAndModify.  SERVER-13883.

const coll = db[jsTestName()];
let ret;
coll.drop();
coll.getDB().createCollection(coll.getName());

// Test update:{} when no documents match the query.
ret = coll.findAndModify({query: {a: 1}, update: {}});
assert.isnull(ret);

// Test update:{} when a document matches the query.  The document is "replaced with the empty
// object" (i.e. all non-_id fields are unset).
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: 1}));
ret = coll.findAndModify({query: {a: 1}, update: {}});
assert.eq(ret, {_id: 0, a: 1});
assert.eq(coll.findOne({_id: 0}), {_id: 0});

// Test update:{} with new:true.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: 1}));
ret = coll.findAndModify({query: {a: 1}, update: {}, new: true});
assert.eq(ret, {_id: 0});
assert.eq(coll.findOne({_id: 0}), {_id: 0});

// Test update:{} with a sort.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: 1}));
assert.commandWorked(coll.insert({_id: 1, a: 1}));
ret = coll.findAndModify({query: {a: 1}, update: {}, sort: {_id: 1}});
assert.eq(ret, {_id: 0, a: 1});
assert.eq(coll.findOne({_id: 0}), {_id: 0});

// Test update:{} with upsert:true.
assert.commandWorked(coll.remove({}));
ret = coll.findAndModify({query: {_id: 0, a: 1}, update: {}, upsert: true});
assert.isnull(ret);
assert.eq(coll.findOne({_id: 0}), {_id: 0});

// Test update:{} with a sort and upsert:true.
assert.commandWorked(coll.remove({}));
ret = coll.findAndModify({query: {_id: 0, a: 1}, update: {}, upsert: true, sort: {a: 1}});
assert.isnull(ret);
assert.eq(coll.findOne({_id: 0}), {_id: 0});

// Test update:{} with a sort, upsert:true, and new:true.
assert.commandWorked(coll.remove({}));
ret =
    coll.findAndModify({query: {_id: 0, a: 1}, update: {}, upsert: true, sort: {a: 1}, new: true});
assert.eq(ret, {_id: 0});
assert.eq(coll.findOne({_id: 0}), {_id: 0});
