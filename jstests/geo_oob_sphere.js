//
// Ensures spherical queries report invalid latitude values in points and center positions
//

t = db.geooobsphere
t.drop();

t.insert({ loc : { x : 30, y : 89 } })
t.insert({ loc : { x : 30, y : 89 } })
t.insert({ loc : { x : 30, y : 89 } })
t.insert({ loc : { x : 30, y : 89 } })
t.insert({ loc : { x : 30, y : 89 } })
t.insert({ loc : { x : 30, y : 89 } })
t.insert({ loc : { x : 30, y : 91 } })

t.ensureIndex({ loc : "2d" })
assert.isnull( db.getLastError() )

assert.throws( function() { t.find({ loc : { $nearSphere : [ 30, 91 ], $maxDistance : 0.25 } }).count() } );
var err = db.getLastError()
assert( err != null )
printjson( err )

assert.throws( function() { t.find({ loc : { $nearSphere : [ 30, 89 ], $maxDistance : 0.25 } }).count() } );
var err = db.getLastError()
assert( err != null )
printjson( err )

assert.throws( function() { t.find({ loc : { $within : { $centerSphere : [[ -180, -91 ], 0.25] } } }).count() } );
var err = db.getLastError()
assert( err != null )
printjson( err )

db.runCommand({ geoNear : "geooobsphere", near : [179, -91], maxDistance : 0.25, spherical : true })
var err = db.getLastError()
assert( err != null )
printjson( err )

db.runCommand({ geoNear : "geooobsphere", near : [30, 89], maxDistance : 0.25, spherical : true })
var err = db.getLastError()
assert( err != null )
printjson( err )