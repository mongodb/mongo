// With invalid geometry, error message should include _id
// SERVER-8992
t = db.geo_invalid_polygon;
t.drop();

// Self-intersecting polygon, triggers
// "Exterior shell of polygon is invalid".
var geometry = {type: "Polygon", coordinates: [[[0, 0], [0, 1], [1, 1], [-2, -1], [0, 0]]]};

t.insert({_id: 42, geometry: geometry});
var err = t.createIndex({geometry: '2dsphere'});
assert.commandFailed(err);

// Document's _id should be in error message.
assert(-1 != err.errmsg.indexOf('42'),
       "Error message didn't contain document _id.\nMessage: \"" + err.errmsg + '"\n');
