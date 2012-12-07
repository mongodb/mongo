// SERVER-7322
t = db.geo_exactfetch
t.drop();

t.insert({ city: "B", lon_lat: [-71.34895, 42.46037], population: 1000})
t.insert({ city: "A", lon_lat: [1.48736, 42.55327], population: 100})

assert.eq(1, t.find({lon_lat: [-71.34895, 42.46037]}).itcount());

t.ensureIndex({lon_lat: "2d", population: -1})

assert.eq(2, t.find({lon_lat: {$near: [-71.34895, 42.46037]}}).itcount());
assert.eq(1, t.find({lon_lat: [-71.34895, 42.46037]}).itcount());
