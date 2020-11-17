t = db.geof;
t.drop();

// corners (dist ~0.98)
// @tags: [
//   sbe_incompatible,
// ]
t.insert({loc: [0.7, 0.7]});
t.insert({loc: [0.7, -0.7]});
t.insert({loc: [-0.7, 0.7]});
t.insert({loc: [-0.7, -0.7]});

// on x axis (dist == 0.9)
t.insert({loc: [-0.9, 0]});
t.insert({loc: [-0.9, 0]});

t.createIndex({loc: "2d"});

t.find({loc: {$near: [0, 0]}}).limit(2).forEach(function(o) {
    // printjson(o);
    assert.lt(Geo.distance([0, 0], o.loc), 0.95);
});
