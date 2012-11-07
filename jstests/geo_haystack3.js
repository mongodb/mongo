t = db.geo_haystack3
t.drop()

t.insert({ pos : { long : 34, lat : 33 }})
t.insert({ pos : { long : 34.2, lat : 33.3 }, type : ["bar", "restaurant" ]})
t.insert({ pos : { long : 34.2, lat : 37.3 }, type : ["bar", "chicken" ]})
t.insert({ pos : { long : 59.1, lat : 87.2 }, type : ["baz", "office" ]})
t.ensureIndex({ pos : "geoHaystack", type : 1 }, { bucketSize : 1 })

// This only matches the first insert.  What do we want?  First 3 or just the first?
res = t.runCommand("geoSearch", { near : [33, 33], maxDistance : 6, search : {}, limit : 30 })
assert.eq(1, res.stats.n, "Right # of matches");
assert.eq(34, res.results[0].pos.long, "expected longitude");
assert.eq(33, res.results[0].pos.lat, "expected latitude");

// This matches the middle 2 of the 4 elements above.
res = t.runCommand("geoSearch", { near : [33, 33], maxDistance : 6, search : { type : "bar" },
                                  limit : 2 })
assert.eq(2, res.stats.n, "Right # of matches");
assert.eq("bar", res.results[0].type[0], "expected value for type");
assert.eq("bar", res.results[1].type[0], "expected value for type");
assert.neq(res.results[0].type[1], res.results[1].type[1], "should get 2 diff results");

// This is a test for the limit being reached/only 1 returned.
res = t.runCommand("geoSearch", { near : [33, 33], maxDistance : 6, search : { type : "bar" },
                                  limit : 1 })
assert.eq(1, res.stats.n, "Right # of matches");
assert.eq("bar", res.results[0].type[0], "expected value for type");
