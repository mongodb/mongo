// Basic examples for $bit
var coll = db.update_bit;
coll.drop();

// $bit and
coll.remove({})
coll.save({_id:1, a:NumberInt(2)});
coll.update({}, {$bit: {a: {and: NumberInt(4)}}})
assert.gleSuccess(coll.getDB())
assert.eq(coll.findOne().a, 0)

// $bit or
coll.remove({})
coll.save({_id:1, a:NumberInt(2)});
coll.update({}, {$bit: {a: {or: NumberInt(4)}}})
assert.gleSuccess(coll.getDB())
assert.eq(coll.findOne().a, 6)

// $bit xor
coll.remove({})
coll.save({_id:1, a:NumberInt(0)});
coll.update({}, {$bit: {a: {xor: NumberInt(4)}}})
assert.gleSuccess(coll.getDB())
assert.eq(coll.findOne().a, 4)
