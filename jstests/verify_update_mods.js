// Verify update mods exist
t = db.update_mods;
t.drop();

t.save({_id:1});
t.update({}, {$set:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$unset:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$inc:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$mul:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$push:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$pushAll:{a:[1]}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$addToSet:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$pull:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$pop:{a:true}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$rename:{a:"b"}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$bit:{a:{and:NumberLong(1)}}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$currentDate:{a:true}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$max:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove()

t.save({_id:1});
t.update({}, {$min:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove()
