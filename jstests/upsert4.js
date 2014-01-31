// tests to ensure fields in $and conditions are created when using the query to do upsert
coll = db.upsert4;
coll.drop();

coll.update({_id: 1, $and: [{c: 1}, {d: 1}], a: 12} , {$inc: {y: 1}} , true);
assert.gleSuccess(db, "");
assert.docEq(coll.findOne(), {_id: 1, c: 1, d: 1, a: 12, y: 1})

coll.remove()
coll.update({$and: [{c: 1}, {d: 1}]} , {$setOnInsert: {_id: 1}} , true);
assert.gleSuccess(db, "");
assert.docEq(coll.findOne(), {_id: 1, c: 1, d: 1})

coll.remove()
coll.update({$and: [{c: 1}, {d: 1}, {$or: [{x:1}]}]} , {$setOnInsert: {_id: 1}} , true);
assert.gleSuccess(db, "");
assert.docEq(coll.findOne(), {_id: 1, c: 1, d: 1, x:1})

coll.remove()
coll.update({$and: [{c: 1}, {d: 1}], $or: [{x: 1}, {x: 2}]} , {$setOnInsert: {_id: 1}} , true);
assert.gleSuccess(db, "");
assert.docEq(coll.findOne(), {_id: 1, c: 1, d: 1})

coll.remove()
coll.update({r: {$gt: 3}, $and: [{c: 1}, {d: 1}], $or: [{x:1}, {x:2}]} , {$setOnInsert: {_id: 1}} , true);
assert.gleSuccess(db, "");
assert.docEq(coll.findOne(), {_id: 1, c: 1, d: 1})

coll.remove()
coll.update({r: /s/, $and: [{c: 1}, {d: 1}], $or: [{x:1}, {x:2}]} , {$setOnInsert: {_id: 1}} , true);
assert.gleSuccess(db, "");
assert.docEq(coll.findOne(), {_id: 1, c: 1, d: 1})

coll.remove()
coll.update({c:2, $and: [{c: 1}, {d: 1}]} , {$setOnInsert: {_id: 1}} , true);
assert.gleError(db, "");
