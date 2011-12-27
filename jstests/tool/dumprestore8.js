// dumprestore8.js

// This file tests that indexes and capped collection options get properly dumped and restored.
// It checks that this works both when doing a full database dump/restore and when doing it just for a single db or collection

t = new ToolTest( "dumprestore8" );

t.startDB( "foo" );
db = t.db;

dbname = db.getName();
dbname2 = "NOT_"+dbname;

db.dropDatabase();

assert.eq( 0 , db.foo.count() , "setup1" );
db.foo.save( { a : 1, b : 1 } );
db.foo.ensureIndex({a:1});
db.foo.ensureIndex({b:1, _id:-1});
assert.eq( 1 , db.foo.count() , "setup2" );


assert.eq( 0 , db.bar.count() , "setup3" );
db.createCollection("bar", {capped:true, size:1000});

for (var i = 0; i < 1000; i++) {
    db.bar.save( { x : i } );
}
db.bar.ensureIndex({x:1});

barDocCount = db.bar.count();
assert.gt( barDocCount, 0 , "No documents inserted" );
assert.lt( db.bar.count(), 1000 , "Capped collection didn't evict documents" );
assert.eq( 4 , db.system.indexes.count() , "Indexes weren't created right" );


// Full dump/restore

t.runTool( "dump" , "--out" , t.ext );

db.dropDatabase();
assert.eq( 0 , db.foo.count() , "foo not dropped" );
assert.eq( 0 , db.bar.count() , "bar not dropped" );
assert.eq( 0 , db.system.indexes.count() , "indexes not dropped" );

t.runTool( "restore" , "--dir" , t.ext );

assert.soon( "db.foo.findOne()" , "no data after sleep" );
assert.eq( 1 , db.foo.count() , "wrong number of docs restored to foo" );
assert.eq( barDocCount, db.bar.count(), "wrong number of docs restored to bar" );
for (var i = 0; i < 10; i++) {
    db.bar.save({x:i});
}
assert.eq( barDocCount, db.bar.count(), "Capped collection didn't evict documents after restore." );
assert.eq( 4 , db.system.indexes.count() , "Indexes weren't created correctly by restore" );


// Dump/restore single DB

dumppath = t.ext + "singledbdump/";
mkdir(dumppath);
t.runTool( "dump" , "-d", dbname, "--out" , dumppath );

db.dropDatabase();
assert.eq( 0 , db.foo.count() , "foo not dropped2" );
assert.eq( 0 , db.bar.count() , "bar not dropped2" );
assert.eq( 0 , db.system.indexes.count() , "indexes not dropped2" );

t.runTool( "restore" , "-d", dbname2, "--dir" , dumppath + dbname );

db = db.getSiblingDB(dbname2);

assert.soon( "db.foo.findOne()" , "no data after sleep 2" );
assert.eq( 1 , db.foo.count() , "wrong number of docs restored to foo 2" );
assert.eq( barDocCount, db.bar.count(), "wrong number of docs restored to bar 2" );
for (var i = 0; i < 10; i++) {
    db.bar.save({x:i});
}
assert.eq( barDocCount, db.bar.count(), "Capped collection didn't evict documents after restore 2." );
assert.eq( 4 , db.system.indexes.count() , "Indexes weren't created correctly by restore 2" );


// Dump/restore single collection

dumppath = t.ext + "singlecolldump/";
mkdir(dumppath);
t.runTool( "dump" , "-d", dbname2, "-c", "bar", "--out" , dumppath );

db.dropDatabase();
assert.eq( 0 , db.bar.count() , "bar not dropped3" );
assert.eq( 0 , db.system.indexes.count() , "indexes not dropped3" );

t.runTool( "restore" , "-d", dbname, "-c", "baz", "--dir" , dumppath + dbname2 + "/bar.bson" );

db = db.getSiblingDB(dbname);

assert.soon( "db.baz.findOne()" , "no data after sleep 2" );
assert.eq( barDocCount, db.baz.count(), "wrong number of docs restored to bar 2" );
for (var i = 0; i < 10; i++) {
    db.baz.save({x:i});
}
assert.eq( barDocCount, db.baz.count(), "Capped collection didn't evict documents after restore 3." );
assert.eq( 1 , db.system.indexes.count() , "Indexes weren't created correctly by restore 3" );

t.stop();
