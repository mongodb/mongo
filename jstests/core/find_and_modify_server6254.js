
t = db.find_and_modify_server6254;
t.drop();

t.insert({x: 1});
ret = t.findAndModify({query: {x: 1}, update: {$set: {x: 2}}, new: true});
assert.eq(2, ret.x, tojson(ret));

assert.eq(1, t.count());
