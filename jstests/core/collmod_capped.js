// Basic js tests for the collMod command.
// Test setting the cappedSize, cappedMaxDocs 
// Run test: mongo jstests/core/collmod_capped.js
// need start mongod with WiredTiger storage engine

function debug( x ) {
    //printjson( x );
}

var coll = "collModTest";
var t = db.getCollection( coll );
t.drop();

var maxSize = 104857600;
var maxDocs = 5;
db.createCollection( coll, {capped: true, size: maxSize, max: maxDocs} );

// test maxDocs
var t = db.getCollection( coll );

for (var i = 0; i < 100; i++) {
   t.insert({x: i});
}

assert.eq(t.count(), maxDocs);
assert(t.stats().size <= maxSize);

// increase maxDocs limit
var maxDocs = 10;
assert.commandWorked(
        db.runCommand({collMod: coll, cappedMaxDocs: maxDocs})
        );

for (var i = 0; i < 1000; i++) {
   t.insert({x: i});
}

assert.eq(t.count(), maxDocs);
assert(t.stats().size <= maxSize);

// decrease maxDocs limit
var maxDocs = 5;
assert.commandWorked(
        db.runCommand({collMod: coll, cappedMaxDocs: maxDocs})
        );

for (var i = 0; i < 1000; i++) {
   t.insert({x: i});
}

assert.eq(t.count(), maxDocs);
assert(t.stats().size <= maxSize);

t.drop();

var maxSize = 4096;
var maxDocs = -1;
db.createCollection( coll, {capped: true, size: maxSize, max: maxDocs} );

// test maxSize
var t = db.getCollection( coll );

for (var i = 0; i < 1000; i++) {
   t.insert({x: i});
}

assert(t.count() < 1000);
assert(t.stats().size <= maxSize);

// increase maxSize
var maxSize = 8192;
assert.commandWorked(
        db.runCommand({collMod: coll, cappedSize: maxSize})
        );

var before = t.count();
for (var i = 0; i < 1000; i++) {
   t.insert({x: i});
}
var after = t.count();

assert(t.count() < 1000);
assert(before < after);
assert(t.stats().size <= maxSize);

// decrease maxSize
var maxSize = 4096;
assert.commandWorked(
        db.runCommand({collMod: coll, cappedSize: maxSize})
        );

var before = t.count();
for (var i = 0; i < 1000; i++) {
   t.insert({x: i});
}
var after = t.count();

assert(t.count() < 1000);
assert(before > after);
assert(t.stats().size <= maxSize);









