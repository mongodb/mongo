// Disallow $ in field names - SERVER-3730

t = db.jstest_updateh
t.drop()

t.insert( {x:1} )

t.update( {x:1}, {$set: {y:1}} ) // ok
e = db.getLastErrorObj()
assert.eq( e.err, null )

t.update( {x:1}, {$set: {$z:1}} ) // not ok
e = db.getLastErrorObj()
assert( e.err != null )

// TODO: This shouldn't be supported, and it isn't with the new update framework, but we
// currently don't have a good way to check which mode we are in. When we do have that, add
// this test guarded under that condition. Or, when we remove the old update path just enable
// this test.
//
// t.update( {x:1}, {$set: {'a.$b':1}} ) // not ok
// e = db.getLastErrorObj()
// assert( e.err != null )

t.update( {x:1}, {$unset: {$z:1}} ) // unset ok to remove bad fields
e = db.getLastErrorObj()
assert.eq( e.err, null )

t.update( {x:1}, {$inc: {$z:1}} ) // not ok
e = db.getLastErrorObj()
assert( e.err != null )

t.update( {x:1}, {$pushAll: {$z:[1,2,3]}} ) // not ok
e = db.getLastErrorObj()
assert( e.err != null )

t.update( {x:1}, {$pushAll: {z:[1,2,3]}} ) // ok
e = db.getLastErrorObj()
assert.eq( e.err, null )
