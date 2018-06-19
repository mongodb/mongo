// Tests geo near with 2 points diametrically opposite to each other
// on the equator
// First reported in SERVER-11830 as a regression in 2.5
(function() {
    var t = db.geos2nearequatoropposite;

    t.drop();

    t.insert({loc: {type: 'Point', coordinates: [0, 0]}});
    t.insert({loc: {type: 'Point', coordinates: [-1, 0]}});

    t.ensureIndex({loc: '2dsphere'});

    // upper bound for half of earth's circumference in meters
    var dist = 40075000 / 2 + 1;

    var nearSphereCount =
        t.find({
             loc: {
                 $nearSphere:
                     {$geometry: {type: 'Point', coordinates: [180, 0]}, $maxDistance: dist}
             }
         }).itcount();
    var nearCount =
        t.find({
             loc: {$near: {$geometry: {type: 'Point', coordinates: [180, 0]}, $maxDistance: dist}}
         }).itcount();
    var geoNearResult = t.aggregate([
                             {
                               $geoNear: {
                                   near: {type: 'Point', coordinates: [180, 0]},
                                   spherical: true,
                                   distanceField: "dist",
                               }
                             },
                             {
                               $group: {
                                   _id: null,
                                   nResults: {$sum: 1},
                                   maxDistance: {$max: "$distanceField"},
                               }
                             }
                         ]).toArray();

    assert.eq(2, nearSphereCount, 'unexpected document count for nearSphere');
    assert.eq(2, nearCount, 'unexpected document count for near');
    assert.eq(1, geoNearResult.length, `unexpected $geoNear result: ${tojson(geoNearResult)}`);

    const geoNearStats = geoNearResult[0];
    assert.eq(2,
              geoNearStats.nResults,
              `unexpected document count for $geoNear: ${tojson(geoNearStats)}`);
    assert.gt(dist,
              geoNearStats.maxDistance,
              `unexpected maximum distance in $geoNear results: ${tojson(geoNearStats)}`);
}());
