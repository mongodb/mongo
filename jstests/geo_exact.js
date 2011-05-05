//
//  Exact checks in geo-index
//

var t = db.exactCheck
t.drop()

t.insert( { loc : [ 1, 2 ], a : 1 } )
t.insert( { loc : { x : 1, y : 2 }, b : 1 } )
t.insert( { loc : [ [ 1, 2 ], { x : 1, y : 2 } ], b : 1 } )
t.insert( { loc : [ { x : 1, y : 2 }, { x : 1, y : 2 } ], a : 1 } )

t.ensureIndex( { loc : "2d" } )
assert.isnull( db.getLastError() )

assert.eq( 4, t.find( { loc : [ 1, 2 ] } ).count() )
assert.eq( 4, t.find( { loc : { x : 1, y : 2 } } ).count() )
assert.eq( "Geo", t.find( { loc : { x : 1, y : 2 } } ).explain().cursor.substring( 0, 3 ) )

assert.eq( 2, t.find( { loc : [ 1, 2 ], a : 1 } ).count() )
assert.eq( 2, t.find( { loc : { x : 1, y : 2 }, b : 1 } ).count() )
assert.eq( "Geo", t.find( { loc : { x : 1, y : 2 }, b : 1 } ).explain().cursor.substring( 0, 3 ) )

t.ensureIndex( { loc : 1 } )
assert.isnull( db.getLastError() )

// Count does not honor hint, so need to count otherwise
assert.eq( 4, t.find( { loc : [ 1, 2 ] } ).hint( { loc : "2d" } ).toArray().length )
assert.eq( 4, t.find( { loc : { x : 1, y : 2 } } ).hint( { loc : "2d" } ).toArray().length )
assert.eq( "Geo", t.find( { loc : { x : 1, y : 2 } } ).hint( { loc : "2d" } ).explain().cursor.substring( 0, 3 ) )

assert.eq( 2, t.find( { loc : [ 1, 2 ], a : 1 } ).hint( { loc : "2d" } ).toArray().length )
assert.eq( 2, t.find( { loc : { x : 1, y : 2 }, b : 1 } ).hint( { loc : "2d" } ).toArray().length )
assert.eq( "Geo", t.find( { loc : { x : 1, y : 2 }, b : 1 } ).hint( { loc : "2d" } ).explain().cursor.substring( 0, 3 ) )
				
				