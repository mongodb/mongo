// Was reported as SERVER-1283.
// The problem seems to be that sometimes the index btrees are such that
// the first search for a matching point in the geo code could run to
// the end of the btree and not reverse direction (leaving the rest of
// the search always looking at some random non-matching point).

t = db.geo_box;
t.drop();

t.insert({"_id": 1, "geo": [33, -11.1]});
t.insert({"_id": 2, "geo": [-122, 33.3]});
t.insert({"_id": 3, "geo": [-122, 33.4]});
t.insert({"_id": 4, "geo": [-122.28, 37.67]});
t.insert({"_id": 5, "geo": [-122.29, 37.68]});
t.insert({"_id": 6, "geo": [-122.29, 37.67]});
t.insert({"_id": 7, "geo": [-122.29, 37.67]});
t.insert({"_id": 8, "geo": [-122.29, 37.68]});
t.insert({"_id": 9, "geo": [-122.29, 37.68]});
t.insert({"_id": 10, "geo": [-122.3, 37.67]});
t.insert({"_id": 11, "geo": [-122.31, 37.67]});
t.insert({"_id": 12, "geo": [-122.3, 37.66]});
t.insert({"_id": 13, "geo": [-122.2435, 37.637072]});
t.insert({"_id": 14, "geo": [-122.289505, 37.695774]});

t.ensureIndex({geo: "2d"});

c = t.find({geo: {"$within": {"$box": [[-125.078461, 36.494473], [-120.320648, 38.905199]]}}});
assert.eq(11, c.count(), "A1");

c = t.find({geo: {"$within": {"$box": [[-124.078461, 36.494473], [-120.320648, 38.905199]]}}});
assert.eq(11, c.count(), "B1");
