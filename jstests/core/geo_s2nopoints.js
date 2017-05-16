// Cannot implicitly shard accessed collections because of use of $near query instead of geoNear
// command.
// @tags: [assumes_unsharded_collection]

// See SERVER-7794.
t = db.geo_s2nopoints;
t.drop();

t.ensureIndex({loc: "2dsphere", x: 1});
assert.eq(
    0,
    t.count({loc: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: 10}}}));
