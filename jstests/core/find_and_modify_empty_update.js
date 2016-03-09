// Test passing update:{} to findAndModify.  SERVER-13883.

var coll = db.find_and_modify_empty_update;
var ret;
coll.drop();
coll.getDB().createCollection(coll.getName());

// Test update:{} when no documents match the query.
ret = coll.findAndModify({query: {a: 1}, update: {}});
assert.isnull(ret);

// Test update:{} when a document matches the query.  The document is "replaced with the empty
// object" (i.e. all non-_id fields are unset).
coll.remove({});
assert.writeOK(coll.insert({_id: 0, a: 1}));
ret = coll.findAndModify({query: {a: 1}, update: {}});
assert.eq(ret, {_id: 0, a: 1});
assert.eq(coll.findOne({_id: 0}), {_id: 0});

// Test update:{} with new:true.
coll.remove({});
assert.writeOK(coll.insert({_id: 0, a: 1}));
ret = coll.findAndModify({query: {a: 1}, update: {}, new: true});
assert.eq(ret, {_id: 0});
assert.eq(coll.findOne({_id: 0}), {_id: 0});

// Test update:{} with a sort.
coll.remove({});
assert.writeOK(coll.insert({_id: 0, a: 1}));
assert.writeOK(coll.insert({_id: 1, a: 1}));
ret = coll.findAndModify({query: {a: 1}, update: {}, sort: {_id: 1}});
assert.eq(ret, {_id: 0, a: 1});
assert.eq(coll.findOne({_id: 0}), {_id: 0});

// Test update:{} with upsert:true.
coll.remove({});
ret = coll.findAndModify({query: {_id: 0, a: 1}, update: {}, upsert: true});
assert.isnull(ret);
assert.eq(coll.findOne({_id: 0}), {_id: 0});

// Test update:{} with a sort and upsert:true.
coll.remove({});
ret = coll.findAndModify({query: {_id: 0, a: 1}, update: {}, upsert: true, sort: {a: 1}});
assert.isnull(ret);
assert.eq(coll.findOne({_id: 0}), {_id: 0});

// Test update:{} with a sort, upsert:true, and new:true.
coll.remove({});
ret =
    coll.findAndModify({query: {_id: 0, a: 1}, update: {}, upsert: true, sort: {a: 1}, new: true});
assert.eq(ret, {_id: 0});
assert.eq(coll.findOne({_id: 0}), {_id: 0});
