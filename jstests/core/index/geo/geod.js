// @tags: [
//   requires_getmore,
// ]

let t = db.geod;
t.drop();
t.save({loc: [0, 0]});
t.save({loc: [0.5, 0]});
t.createIndex({loc: "2d"});
// do a few geoNears with different maxDistances.  The first iteration
// should match no points in the dataset.
let dists = [0.49, 0.51, 1.0];
for (let idx in dists) {
    let b = db.geod
        .aggregate([{$geoNear: {near: [1, 0], distanceField: "d", maxDistance: dists[idx]}}, {$limit: 2}])
        .toArray();
    assert.eq(b.length, idx, "B" + idx);
}
