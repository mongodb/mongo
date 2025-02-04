const t = db[jsTestName()];
t.drop();

const ret = t.findAndModify({query: {_id: 1}, update: {"$inc": {i: 1}}, upsert: true});
assert.isnull(ret);
