// recstore.js
// this is a simple test for new recstores (see reci.h)
// it is probably redundant with other tests but is a convenient starting point
// for testing such things.

t = db.storetest;

t.drop();

t.save({z:3});
t.save({z:2});

t.ensureIndex({z:1});
t.ensureIndex({q:1});
assert( t.find().sort({z:1})[0].z == 2 );

t.dropIndexes();

assert( t.find().sort({z:1})[0].z == 2 );

t.ensureIndex({z:1});
t.ensureIndex({q:1});

db.getSisterDB('admin').$cmd.findOne({closeAllDatabases:1});
