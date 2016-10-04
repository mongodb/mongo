t = db.geo_s2multi;
t.drop();

t.ensureIndex({geo: "2dsphere"});

// Let's try the examples in the GeoJSON spec.
multiPointA = {
    "type": "MultiPoint",
    "coordinates": [[100.0, 0.0], [101.0, 1.0]]
};
assert.writeOK(t.insert({geo: multiPointA}));

multiLineStringA = {
    "type": "MultiLineString",
    "coordinates": [[[100.0, 0.0], [101.0, 1.0]], [[102.0, 2.0], [103.0, 3.0]]]
};
assert.writeOK(t.insert({geo: multiLineStringA}));

multiPolygonA = {
    "type": "MultiPolygon",
    "coordinates": [
        [[[102.0, 2.0], [103.0, 2.0], [103.0, 3.0], [102.0, 3.0], [102.0, 2.0]]],
        [
          [[100.0, 0.0], [101.0, 0.0], [101.0, 1.0], [100.0, 1.0], [100.0, 0.0]],
          [[100.2, 0.2], [100.8, 0.2], [100.8, 0.8], [100.2, 0.8], [100.2, 0.2]]
        ]
    ]
};
assert.writeOK(t.insert({geo: multiPolygonA}));

assert.eq(3, t.find({
                  geo: {$geoIntersects: {$geometry: {"type": "Point", "coordinates": [100, 0]}}}
              }).itcount());
assert.eq(3,
          t.find({
               geo: {$geoIntersects: {$geometry: {"type": "Point", "coordinates": [101.0, 1.0]}}}
           }).itcount());

// Inside the hole in multiPolygonA
assert.eq(
    0, t.find({
            geo: {$geoIntersects: {$geometry: {"type": "Point", "coordinates": [100.21, 0.21]}}}
        }).itcount());

// One point inside the hole, one out.
assert.eq(
    3,
    t.find({
         geo: {
             $geoIntersects:
                 {$geometry: {"type": "MultiPoint", "coordinates": [[100, 0], [100.21, 0.21]]}}
         }
     }).itcount());
assert.eq(
    3,
    t.find({
         geo: {
             $geoIntersects: {
                 $geometry:
                     {"type": "MultiPoint", "coordinates": [[100, 0], [100.21, 0.21], [101, 1]]}
             }
         }
     }).itcount());
// Polygon contains itself and the multipoint.
assert.eq(2, t.find({geo: {$geoWithin: {$geometry: multiPolygonA}}}).itcount());

partialPolygonA = {
    "type": "Polygon",
    "coordinates": [[[102.0, 2.0], [103.0, 2.0], [103.0, 3.0], [102.0, 3.0], [102.0, 2.0]]]
};
assert.writeOK(t.insert({geo: partialPolygonA}));
// Polygon contains itself, the partial poly, and the multipoint
assert.eq(3, t.find({geo: {$geoWithin: {$geometry: multiPolygonA}}}).itcount());

assert.eq(1, t.find({geo: {$geoWithin: {$geometry: partialPolygonA}}}).itcount());

// Itself, the multi poly, the multipoint...
assert.eq(3, t.find({geo: {$geoIntersects: {$geometry: partialPolygonA}}}).itcount());
