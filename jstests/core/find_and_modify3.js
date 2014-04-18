t = db.find_and_modify3;
t.drop();

t.insert({_id:0, other:0, comments:[{i:0, j:0}, {i:1, j:1}]});
t.insert({_id:1, other:1, comments:[{i:0, j:0}, {i:1, j:1}]}); // this is the only one that gets modded
t.insert({_id:2, other:2, comments:[{i:0, j:0}, {i:1, j:1}]});

orig0 = t.findOne({_id:0})
orig2 = t.findOne({_id:2})

out = t.findAndModify({query: {_id:1, 'comments.i':0}, update: {$set: {'comments.$.j':2}}, 'new': true, sort:{other:1}});
assert.eq(out.comments[0], {i:0, j:2});
assert.eq(out.comments[1], {i:1, j:1});
assert.eq(t.findOne({_id:0}), orig0);
assert.eq(t.findOne({_id:2}), orig2);

out = t.findAndModify({query: {other:1, 'comments.i':1}, update: {$set: {'comments.$.j':3}}, 'new': true, sort:{other:1}});
assert.eq(out.comments[0], {i:0, j:2});
assert.eq(out.comments[1], {i:1, j:3});
assert.eq(t.findOne({_id:0}), orig0);
assert.eq(t.findOne({_id:2}), orig2);
