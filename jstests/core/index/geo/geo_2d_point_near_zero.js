/**
 * Tests behavior of indexing geo values incredibly close to the upper boundary of the 2d index.
 * See SERVER-92930 for more details.
 */

var coll = db.getCollection(jsTestName());
coll.drop();

coll.createIndex({loc: "2d"}, {max: 0});

coll.insert({loc: [0, 0]});
coll.insert({loc: [-100, -50]});
coll.insert({loc: [-180, -50]});
coll.insert({loc: [0, -50]});

// These documents include values that are so close to 0 that we must make sure we don't treat them
// as wrapping to be equal to -180 (fixed by SERVER-92930).
coll.insert({loc: [-2.220446049250313e-16, 0]});
coll.insert({loc: [-1.0e-14, -50]});
coll.insert({loc: [-5, -5.24e-20]});

assert.eq(coll.find({loc: {$within: {$box: [[-180, -180], [0, 0]]}}}).itcount(), 7);

assert.eq(coll.find({loc: {$within: {$box: [[-10, -10], [10, 10]]}}}).itcount(), 3);

assert.eq(coll.find({loc: {$within: {$box: [[-180, -60], [0, -30]]}}}).itcount(), 4);

assert.eq(coll.find({loc: {$within: {$box: [[-10, -60], [0, -30]]}}}).itcount(), 2);

assert.eq(coll.find({loc: {$within: {$box: [[-5, -60], [0, 0]]}}}).itcount(), 5);
