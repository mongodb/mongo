// Verify update mods exist
t = db.update_mods;
t.drop();

t.save({_id:1});
t.update({}, {$set:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$unset:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$inc:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$mul:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$push:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$pushAll:{a:[1]}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$addToSet:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$pull:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$pop:{a:true}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$rename:{a:"b"}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$bit:{a:{and:NumberLong(1)}}})
assert.automsg( "!db.getLastError()" );
t.remove({})

// SERVER-3223 test $bit can do an upsert
t.update({_id:1}, {$bit:{a:{and:NumberLong(3)}}}, true);
assert.eq(t.findOne({_id:1}).a, NumberLong(0), "$bit upsert with and");
t.update({_id:2}, {$bit:{b:{or:NumberLong(3)}}}, true);
assert.eq(t.findOne({_id:2}).b, NumberLong(3), "$bit upsert with or (long)");
t.update({_id:3}, {$bit:{"c.d":{or:NumberInt(3)}}}, true);
assert.eq(t.findOne({_id:3}).c.d, NumberInt(3), "$bit upsert with or (int)");
t.remove({});

t.save({_id:1});
t.update({}, {$currentDate:{a:true}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$max:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove({})

t.save({_id:1});
t.update({}, {$min:{a:1}})
assert.automsg( "!db.getLastError()" );
t.remove({})
