// Disallow $ in field names - SERVER-3730

t = db.jstest_updateh
t.drop()

t.insert( {x:1} )

t.update( {x:1}, {$set: {y:1}} ) // ok
e = db.getLastErrorObj()
assert.eq( e.err, null )

t.update( {x:1}, {$set: {$z:1}} ) // not ok
e = db.getLastErrorObj()
assert( e.err != null && e.code == 15896 )

t.update( {x:1}, {$unset: {$z:1}} ) // unset ok to remove bad fields
e = db.getLastErrorObj()
assert.eq( e.err, null )

t.update( {x:1}, {$inc: {$z:1}} ) // not ok
e = db.getLastErrorObj()
assert( e.err != null && e.code == 15896 )

t.update( {x:1}, {$pushAll: {$z:[1,2,3]}} ) // not ok
e = db.getLastErrorObj()
assert( e.err != null && e.code == 15896 )

t.update( {x:1}, {$pushAll: {z:[1,2,3]}} ) // ok
e = db.getLastErrorObj()
assert.eq( e.err, null )
