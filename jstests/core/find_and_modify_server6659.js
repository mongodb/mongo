
t = db.find_and_modify_server6659;
t.drop();

x = t.findAndModify({query: {f: 1}, update: {$set: {f: 2}}, upsert: true, new: true});
assert.eq(2, x.f);
assert.eq(2, t.findOne().f);
