// Make sure that the 2dsphere index can deal with non-GeoJSON points.
// 2dsphere does not accept legacy shapes, only legacy points.
t = db.geo_s2indexoldformat
t.drop()

t.insert( {geo : [40, 5], nonGeo: ["pointA"]})
t.insert( {geo : [41.001, 6.001], nonGeo: ["pointD"]})
t.insert( {geo : [41, 6], nonGeo: ["pointB"]})
t.insert( {geo : [41, 6]} )
t.insert( {geo : {x:40.6, y:5.4}} )

t.ensureIndex( { geo : "2dsphere", nonGeo: 1 } )

res = t.find({ "geo" : { "$geoIntersects" : { "$geometry": {x:40, y:5}}}})
assert.eq(res.count(), 1);

res = t.find({ "geo" : { "$geoIntersects" : {"$geometry": [41,6]}}})
assert.eq(res.count(), 2);

// We don't support legacy polygons in 2dsphere.
t.insert( {geo : [[40,5],[40,6],[41,6],[41,5]], nonGeo: ["somepoly"] })
assert(db.getLastError());

t.insert( {geo : {a:{x:40,y:5},b:{x:40,y:6},c:{x:41,y:6},d:{x:41,y:5}}})
assert(db.getLastError());

res = t.find({ "geo" : { "$geoIntersects" : {"$geometry": [[40,5],[40,6],[41,6],[41,5]]}}})
assert(db.getLastError());
