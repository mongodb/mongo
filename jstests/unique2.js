
t = db.foo;

t.drop();

/* test for good behavior when indexing multikeys */

t.insert({k:3});
t.insert({k:[2,3]});
t.insert({k:[4,3]});

t.ensureIndex({k:1}, {unique:true, dropDups:true});

assert( t.count() == 1 ) ;
assert( t.find().sort({k:1}).toArray().length == 1 ) ;
assert( t.find().sort({k:1}).count() == 1 ) ;

t.drop();

t.ensureIndex({k:1}, {unique:true});

t.insert({k:3});
t.insert({k:[2,3]});
t.insert({k:[4,3]});

assert( t.count() == 1 ) ;
assert( t.find().sort({k:1}).toArray().length == 1 ) ;
assert( t.find().sort({k:1}).count() == 1 ) ;

t.dropIndexes();

t.insert({k:[2,3]});
t.insert({k:[4,3]});
assert( t.count() == 3 ) ;

t.ensureIndex({k:1}, {unique:true, dropDups:true});

assert( t.count() == 1 ) ;
assert( t.find().sort({k:1}).toArray().length == 1 ) ;
assert( t.find().sort({k:1}).count() == 1 ) ;

