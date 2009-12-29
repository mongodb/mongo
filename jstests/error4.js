
t = db.error4;
t.drop()
t.insert( { _id : 1 } )
t.insert( { _id : 1 } )
assert.eq( 11000 , db.getLastErrorCmd().code , "A" )

