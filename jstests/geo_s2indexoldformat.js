// Make sure that the 2dsphere index can deal with non-GeoJSON data.
t = db.geo_s2indexoldformat
t.drop()

t.insert( {geo : [40, 5], nonGeo: ["pointA"]})
t.insert( {geo : [41.001, 6.001], nonGeo: ["pointD"]})
t.insert( {geo : [41, 6], nonGeo: ["pointB"]})
t.insert( {geo : [41, 6]} )
t.insert( {geo : {x:40.6, y:5.4}} )

t.insert( {geo : [[40,5],[40,6],[41,6],[41,5]], nonGeo: ["somepoly"] })
t.insert( {geo : {a:{x:40,y:5},b:{x:40,y:6},c:{x:41,y:6},d:{x:41,y:5}}})

t.ensureIndex( { geo : "2dsphere", nonGeo: 1 } )

res = t.find({ "geo" : { "$intersect" : { "$geometry": {x:40, y:5}}}})
assert.eq(res.count(), 3);

res = t.find({ "geo" : { "$intersect" : {"$geometry": [41,6]}}})
assert.eq(res.count(), 4);

res = t.find({ "geo" : { "$intersect" : {"$geometry": [[40,5],[40,6],[41,6],[41,5]]}}})
assert.eq(res.count(), 6);
