
t = db.geo8
t.drop()

t.insert( { loc : [ 5 , 5 ] } )
t.insert( { loc : [ 5 , 6 ] } )
t.insert( { loc : [ 5 , 7 ] } )
t.insert( { loc : [ 4 , 5 ] } )
t.insert( { loc : [ 100 , 100 ] } )

t.ensureIndex( { loc : "2d" } )

t.runCommand( "geoWalk" );
