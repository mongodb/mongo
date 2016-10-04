var t = db.geod;
t.drop();
t.save({loc: [0, 0]});
t.save({loc: [0.5, 0]});
t.ensureIndex({loc: "2d"});
// do a few geoNears with different maxDistances.  The first iteration
// should match no points in the dataset.
dists = [.49, .51, 1.0];
for (idx in dists) {
    b = db.runCommand({geoNear: "geod", near: [1, 0], num: 2, maxDistance: dists[idx]});
    assert.eq(b.errmsg, undefined, "A" + idx);
    l = b.results.length;
    assert.eq(l, idx, "B" + idx);
}
