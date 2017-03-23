// Tests geo queries w/ update & upsert
// from SERVER-3428

var coll = db.testGeoUpdate;
coll.drop();

coll.ensureIndex({loc: "2d"});

// Test normal update
print("Updating...");

coll.insert({loc: [1.0, 2.0]});

coll.update({loc: {$near: [1.0, 2.0]}}, {x: true, loc: [1.0, 2.0]});

// Test upsert
print("Upserting...");

coll.update({loc: {$within: {$center: [[10, 20], 1]}}}, {x: true}, true);

coll.update({loc: {$near: [10.0, 20.0], $maxDistance: 1}}, {x: true}, true);

coll.update({loc: {$near: [100, 100], $maxDistance: 1}},
            {$set: {loc: [100, 100]}, $push: {people: "chris"}},
            true);

coll.update({loc: {$near: [100, 100], $maxDistance: 1}},
            {$set: {loc: [100, 100]}, $push: {people: "john"}},
            true);

assert.eq(4, coll.find().itcount());
