// With invalid geometry, error message should include _id
// SERVER-8992
t = db.geo_invalid_polygon;
t.drop();

// Self-intersecting polygon, triggers
// "Exterior shell of polygon is invalid".
var geometry = {
    type: "Polygon",
    coordinates: [
        [
            [ 0, 0 ],
            [ 0, 1 ],
            [ 1, 1 ],
            [-2,-1 ],
            [ 0, 0 ]
        ]
    ]
};

t.insert({_id: 42, geometry: geometry});
t.createIndex({geometry: '2dsphere'});
var gleResult = db.getLastErrorCmd(1);

// Verify that we triggered the error we're trying to test.
assert.eq(16755, gleResult.code);

// Document's _id should be in error message.
assert(
    -1 != gleResult.err.indexOf('42'),
    "Error message didn't contain document _id.\nMessage: \"" + gleResult.err
    + '"\n'
);
