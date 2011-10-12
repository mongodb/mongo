t = db.factories
t.drop()
t.insert( { name: "xyz", metro: { city: "New York", state: "NY" } } )
t.ensureIndex( { metro : 1 } )

assert( db.factories.find().count() )

assert( db.factories.find( { metro: { city: "New York", state: "NY" } } ).count() )

assert( db.factories.find( { metro: { city: "New York", state: "NY" } } ).explain().cursor == "BtreeCursor metro_1" )

assert( db.factories.find( { metro: { $gte : { city: "New York" } } } ).explain().cursor == "BtreeCursor metro_1" )

assert( db.factories.find( { metro: { $gte : { city: "New York" } } } ).count() == 1 )

