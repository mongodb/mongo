// Check exists with non empty document, based on SERVER-2470 example.

t = db.jstests_exists3;
t.drop();

t.insert({a: 1, b: 2});

assert.eq( 1, t.find({}).sort({c: -1}).itcount() );
assert.eq( 1, t.count({c: {$exists: false}}) );
assert.eq( 1, t.find({c: {$exists: false}}).itcount() );
assert.eq( 1, t.find({c: {$exists: false}}).sort({c: -1}).itcount() );

// now we have an index on the sort key 
t.ensureIndex({c: -1})

assert.eq( 1, t.find({c: {$exists: false}}).sort({c: -1}).itcount() );
assert.eq( 1, t.find({c: {$exists: false}}).itcount() );
// still ok without the $exists 
assert.eq( 1, t.find({}).sort({c: -1}).itcount() );
// and ok with a convoluted $not $exists 
assert.eq( 1, t.find({c: {$not: {$exists: true}}}).sort({c: -1}).itcount() );
