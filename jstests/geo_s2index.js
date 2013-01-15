t = db.geo_s2index
t.drop()

pointA = { "type" : "Point", "coordinates": [ 40, 5 ] }
t.insert( {geo : pointA , nonGeo: "pointA"})

pointD = { "type" : "Point", "coordinates": [ 41.001, 6.001 ] }
t.insert( {geo : pointD , nonGeo: "pointD"})

someline = { "type" : "LineString", "coordinates": [ [ 40, 5], [41, 6]]}
t.insert( {geo : someline , nonGeo: "someline"})

pointB = { "type" : "Point", "coordinates": [ 41, 6 ] }
t.insert( {geo : pointB , nonGeo: "pointB"})

pointC = { "type" : "Point", "coordinates": [ 41, 6 ] }
t.insert( {geo : pointC} )

// Add a point within the polygon but not on the border.  Don't want to be on
// the path of the polyline.
pointE = { "type" : "Point", "coordinates": [ 40.6, 5.4 ] }
t.insert( {geo : pointE} )

somepoly = { "type" : "Polygon",
             "coordinates" : [ [ [40,5], [40,6], [41,6], [41,5], [40,5]]]}
t.insert( {geo : somepoly, nonGeo: "somepoly" })
t.ensureIndex( { geo : "2dsphere", nonGeo: 1 } )

res = t.find({ "geo" : { "$geoIntersects" : { "$geometry" : pointA} } });
assert.eq(res.count(), 3);

res = t.find({ "geo" : { "$geoIntersects" : { "$geometry" : pointB} } });
assert.eq(res.count(), 4);

res = t.find({ "geo" : { "$geoIntersects" : { "$geometry" : pointD} } });
assert.eq(res.count(), 1);

res = t.find({ "geo" : { "$geoIntersects" : { "$geometry" : someline} } })
assert.eq(res.count(), 5);

res = t.find({ "geo" : { "$geoIntersects" : { "$geometry" : somepoly} } })
assert.eq(res.count(), 6);

res = t.find({ "geo" : { "$within" : { "$geometry" : somepoly} } })
assert.eq(res.count(), 6);

res = t.find({ "geo" : { "$geoIntersects" : { "$geometry" : somepoly} } }).limit(1)
assert.eq(res.itcount(), 1);

res = t.find({ "nonGeo": "pointA",
               "geo" : { "$geoIntersects" : { "$geometry" : somepoly} } })
assert.eq(res.count(), 1);

// Don't crash mongod if we give it bad input.
t.drop()
t.ensureIndex({loc: "2dsphere", x:1})
t.save({loc: [0,0]})
assert.throws(function() { return t.count({loc: {$foo:[0,0]}}) })
assert.throws(function() { return t.find({ "nonGeo": "pointA",
                                           "geo" : { "$geoIntersects" : { "$geometry" : somepoly},
                                                     "$near": {"$geometry" : somepoly }}}).count()}) 

// If we specify a datum, it has to be valid (WGS84).
t.drop()
t.ensureIndex({loc: "2dsphere"})
t.insert({loc: {type:'Point', coordinates: [40, 5], crs:{ type: 'name', properties:{name:'EPSG:2000'}}}})
assert(db.getLastError());
assert.eq(0, t.find().itcount())
t.insert({loc: {type:'Point', coordinates: [40, 5]}})
assert(!db.getLastError());
t.insert({loc: {type:'Point', coordinates: [40, 5], crs:{ type: 'name', properties:{name:'EPSG:4326'}}}})
assert(!db.getLastError());
t.insert({loc: {type:'Point', coordinates: [40, 5], crs:{ type: 'name', properties:{name:'urn:ogc:def:crs:OGC:1.3:CRS84'}}}})
assert(!db.getLastError());
