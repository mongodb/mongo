
t = db.server5346;
t.drop();

x = { _id : 1 , versions : {} } 
t.insert( x ) 

t.update({ _id : 1 }, { $inc : { "versions.2_01" : 1 } } )
t.update({ _id : 1 }, { $inc : { "versions.2_1" : 2 } } )
t.update({ _id : 1 }, { $inc : { "versions.01" : 3 } } )
t.update({ _id : 1 }, { $inc : { "versions.1" : 4 } } )

// Make sure the correct fields are set, without duplicates.
// String comparison must be used because V8 reorders numeric fields in objects.
assert.eq( '{ "_id" : 1, "versions" : { "01" : 3, "1" : 4, "2_01" : 1, "2_1" : 2 } }' ,
          tojson( t.findOne(), null, true ) )
