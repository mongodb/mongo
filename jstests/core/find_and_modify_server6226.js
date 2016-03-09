
t = db.find_and_modify_server6226;
t.drop();

ret = t.findAndModify({query: {_id: 1}, update: {"$inc": {i: 1}}, upsert: true});
assert.isnull(ret);
