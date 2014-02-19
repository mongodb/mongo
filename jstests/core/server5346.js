
t = db.server5346;
t.drop();

x = { _id : 1 , versions : {} } 
t.insert( x ) 

t.update({ _id : 1 }, { $inc : { "versions.2_01" : 1 } } )
t.update({ _id : 1 }, { $inc : { "versions.2_1" : 2 } } )
t.update({ _id : 1 }, { $inc : { "versions.01" : 3 } } )
t.update({ _id : 1 }, { $inc : { "versions.1" : 4 } } )

// Make sure the correct fields are set, without duplicates.
assert.docEq( { "_id" : 1, "versions" : { "01" : 3, "1" : 4, "2_01" : 1, "2_1" : 2 } },
              t.findOne())
