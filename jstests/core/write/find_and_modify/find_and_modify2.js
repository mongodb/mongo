const coll = db.find_and_modify2;
coll.drop();

coll.insert({_id: 1, i: 0, j: 0});

let out = coll.findAndModify({update: {$inc: {i: 1}}, 'new': true, fields: {i: 1}});
assert.eq(out, {_id: 1, i: 1});

out = coll.findAndModify({update: {$inc: {i: 1}}, fields: {i: 0}});
assert.eq(out, {_id: 1, j: 0});

out = coll.findAndModify({update: {$inc: {i: 1}}, fields: {_id: 0, j: 1}});
assert.eq(out, {j: 0});

out = coll.findAndModify({update: {$inc: {i: 1}}, fields: {_id: 0, j: 1}, 'new': true});
assert.eq(out, {j: 0});