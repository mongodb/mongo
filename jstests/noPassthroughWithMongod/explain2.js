// Test for race condition SERVER-2807.  One cursor is dropped and another is not.

collName = 'jstests_slowNightly_explain2';

t = db[collName];
t.drop();

db.createCollection(collName, {capped: true, size: 100000});
t = db[collName];
t.ensureIndex({x: 1});

a = startParallelShell('for( i = 0; i < 50000; ++i ) { db.' + collName + '.insert( {x:i,y:1} ); }');

for (i = 0; i < 800; ++i) {
    t.find({x: {$gt: -1}, y: 1}).sort({x: -1}).explain();
}

a();