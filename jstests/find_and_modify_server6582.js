
t = db.find_and_modify_server6582;

t.drop();
x = t.runCommand( "findAndModify" , {query:{f:1}, update:{$set:{f:2}}, upsert:true, new:true})
le = x.lastErrorObject
assert.eq( le.updatedExisting, false )
assert.eq( le.n, 1 )
assert.eq( le.upserted, x.value._id )

t.drop();
t.insert( { f : 1 } )
x = t.runCommand( "findAndModify" , {query:{f:1}, remove : true } )
le = x.lastErrorObject
assert.eq( le.n, 1 )



