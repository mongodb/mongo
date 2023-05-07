// SERVER-9484
// A geometry may have several covers, one of which is in a search ring and the other of which is
// not.  If we see the cover that's not in the search ring, we can't mark the object as 'seen' for
// this ring.
let t = db.geo_s2nearcorrect;
t.drop();

let longline = {"type": "LineString", "coordinates": [[0, 0], [179, 89]]};
t.insert({geo: longline});
t.createIndex({geo: "2dsphere"});
let origin = {"type": "Point", "coordinates": [45, 45]};
assert.eq(1, t.find({"geo": {"$near": {"$geometry": origin, $maxDistance: 20000000}}}).count());
