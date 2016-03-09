// Reproduces simple test for SERVER-2115

// The setup to reproduce is to create a set of points and a really big bounds so that we are
// required to do
// exact lookups on the points to get correct results.

t = db.geo_fiddly_box2;
t.drop();

t.insert({"letter": "S", "position": [-3, 0]});
t.insert({"letter": "C", "position": [-2, 0]});
t.insert({"letter": "R", "position": [-1, 0]});
t.insert({"letter": "A", "position": [0, 0]});
t.insert({"letter": "B", "position": [1, 0]});
t.insert({"letter": "B", "position": [2, 0]});
t.insert({"letter": "L", "position": [3, 0]});
t.insert({"letter": "E", "position": [4, 0]});

t.ensureIndex({position: "2d"});
result = t.find({"position": {"$within": {"$box": [[-3, -1], [0, 1]]}}});
assert.eq(4, result.count());

t.dropIndex({position: "2d"});
t.ensureIndex({position: "2d"}, {min: -10000000, max: 10000000});

result = t.find({"position": {"$within": {"$box": [[-3, -1], [0, 1]]}}});
assert.eq(4, result.count());

t.dropIndex({position: "2d"});
t.ensureIndex({position: "2d"}, {min: -1000000000, max: 1000000000});

result = t.find({"position": {"$within": {"$box": [[-3, -1], [0, 1]]}}});
assert.eq(4, result.count());
