t = db.find_and_modify2;
t.drop();

t.insert({_id:1, i:0, j:0});

out = t.findAndModify({update: {$inc: {i:1}}, 'new': true, fields: {i:1}});
assert.eq(out, {_id:1, i:1});

out = t.findAndModify({update: {$inc: {i:1}}, fields: {i:0}});
assert.eq(out, {_id:1, j:0});

out = t.findAndModify({update: {$inc: {i:1}}, fields: {_id:0, j:1}});
assert.eq(out, {j:0});

out = t.findAndModify({update: {$inc: {i:1}}, fields: {_id:0, j:1}, 'new': true});
assert.eq(out, {j:0});
