// Disallow $ in field names - SERVER-3730
var res;

t = db.jstest_updateh
t.drop()

t.insert( {x:1} )

res = t.update( {x:1}, {$set: {y:1}} ) // ok
assert.writeOK( res )

res = t.update( {x:1}, {$set: {$z:1}} ) // not ok
assert.writeError( res )

// TODO: This shouldn't be supported, and it isn't with the new update framework, but we
// currently don't have a good way to check which mode we are in. When we do have that, add
// this test guarded under that condition. Or, when we remove the old update path just enable
// this test.
//
// res = t.update( {x:1}, {$set: {'a.$b':1}} ) // not ok
// assert.writeError( res )

res = t.update( {x:1}, {$unset: {$z:1}} ) // unset ok to remove bad fields
assert.writeOK( res )

res = t.update( {x:1}, {$inc: {$z:1}} ) // not ok
assert.writeError( res )

res = t.update( {x:1}, {$pushAll: {$z:[1,2,3]}} ) // not ok
assert.writeError( res )

res = t.update( {x:1}, {$pushAll: {z:[1,2,3]}} ) // ok
assert.writeOK( res )
