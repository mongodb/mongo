t = db.geo_s2index
t.drop()

pointA = { "type" : "Point", "coordinates": [ 40, 5 ] }
t.insert( {geo : pointA , nonGeo: ["pointA"]})

pointD = { "type" : "Point", "coordinates": [ 41.001, 6.001 ] }
t.insert( {geo : pointD , nonGeo: ["pointD"]})

someline = { "type" : "LineString", "coordinates": [ [ 40, 5], [41, 6]]}
t.insert( {geo : someline , nonGeo: ["someline"]})

pointB = { "type" : "Point", "coordinates": [ 41, 6 ] }
t.insert( {geo : pointB , nonGeo: ["pointB"]})

pointC = { "type" : "Point", "coordinates": [ 41, 6 ] }
t.insert( {geo : pointC} )

// Add a point within the polygon but not on the border.  Don't want to be on
// the path of the polyline.
pointE = { "type" : "Point", "coordinates": [ 40.6, 5.4 ] }
t.insert( {geo : pointE} )

somepoly = { "type" : "Polygon",
             "coordinates" : [ [ [40,5], [40,6], [41,6], [41,5], [40,5]]]}
t.insert( {geo : somepoly, nonGeo: ["somepoly"] })
t.ensureIndex( { geo : "2dsphere", nonGeo: 1 } )

res = t.find({ "geo" : { "$intersect" : { "$geometry" : pointA} } });
assert.eq(res.count(), 3);

res = t.find({ "geo" : { "$intersect" : { "$geometry" : pointB} } });
assert.eq(res.count(), 4);

res = t.find({ "geo" : { "$intersect" : { "$geometry" : pointD} } });
assert.eq(res.count(), 1);

res = t.find({ "geo" : { "$intersect" : { "$geometry" : someline} } })
assert.eq(res.count(), 5);

res = t.find({ "geo" : { "$intersect" : { "$geometry" : somepoly} } })
assert.eq(res.count(), 6);

res = t.find({ "geo" : { "$intersect" : { "$geometry" : somepoly} } }).limit(1)
assert.eq(res.itcount(), 1);

res = t.find({ "nonGeo": "pointA",
               "geo" : { "$intersect" : { "$geometry" : somepoly} } })
assert.eq(res.count(), 1);

// Don't crash mongod if we give it bad input.
t.drop()
t.ensureIndex({loc: "2dsphere", x:1})
t.save({loc: [0,0]})
assert.throws(function() { return t.count({loc: {$foo:[0,0]}}) })
assert.throws(function() { return t.find({ "nonGeo": "pointA",
                                           "geo" : { "$intersect" : { "$geometry" : somepoly},
                                                     "$near": {"$geometry" : somepoly }}}).count()}) 
