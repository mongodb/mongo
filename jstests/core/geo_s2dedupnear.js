// Make sure that we don't return several of the same result due to faulty
// assumptions about the btree cursor.  That is, don't return duplicate results.
t = db.geo_s2dedupnear
t.drop()

t.ensureIndex( { geo : "2dsphere" } )
var x = { "type" : "Polygon",
             "coordinates" : [ [ [100.0, 0.0], [101.0, 0.0], [101.0, 1.0], [100.0, 1.0], [100.0, 0.0]]]}
t.insert({geo: x})
res = t.find({geo: {$geoNear: {"type" : "Point", "coordinates" : [31, 41]}}})
assert.eq(res.itcount(), 1)
