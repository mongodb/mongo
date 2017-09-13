// SERVER-6366
// relates to SERVER-808
//
// This file tests that options are not restored upon
// mongorestore with --noOptionsRestore
//
// It checks that this works both when doing a full
// database dump/restore and when doing it just for a
// single db or collection.

t = new ToolTest("dumprestoreWithNoOptions");

t.startDB("foo");
db = t.db;

// We turn this off to prevent the server from touching the 'options' field in system.namespaces.
// This is important because we check exact values of the 'options' field in this test.
db.adminCommand({setParameter: 1, newCollectionsUsePowerOf2Sizes: false});

dbname = db.getName();
dbname2 = "NOT_" + dbname;

db.dropDatabase();

var defaultFlags = {};

var options = {capped: true, size: 4096, autoIndexId: true};
db.createCollection('capped', options);
assert.eq(1, db.capped.getIndexes().length, "auto index not created");
var cappedOptions = db.capped.exists().options;
for (var opt in options) {
    assert.eq(options[opt],
              cappedOptions[opt],
              'invalid option:' + tojson(options) + " " + tojson(cappedOptions));
}
assert.writeOK(db.capped.insert({x: 1}));

// Full dump/restore

t.runTool("dump", "--out", t.ext);

db.dropDatabase();
assert.eq(0, db.capped.count(), "capped not dropped");
assert.eq(0, db.capped.getIndexes().length, "indexes not dropped");

t.runTool("restore", "--dir", t.ext, "--noOptionsRestore");

assert.eq(1, db.capped.count(), "wrong number of docs restored to capped");
assert(true !== db.capped.stats().capped, "restore options were not ignored");
assert.eq(defaultFlags,
          db.capped.exists().options,
          "restore options not ignored: " + tojson(db.capped.exists()));

// Dump/restore single DB

db.dropDatabase();
var options = {capped: true, size: 4096, autoIndexId: true};
db.createCollection('capped', options);
assert.eq(1, db.capped.getIndexes().length, "auto index not created");
var cappedOptions = db.capped.exists().options;
for (var opt in options) {
    assert.eq(options[opt], cappedOptions[opt], 'invalid option');
}
assert.writeOK(db.capped.insert({x: 1}));

dumppath = t.ext + "noOptionsSingleDump/";
mkdir(dumppath);
t.runTool("dump", "-d", dbname, "--out", dumppath);

db.dropDatabase();
assert.eq(0, db.capped.count(), "capped not dropped");
assert.eq(0, db.capped.getIndexes().length, "indexes not dropped");

t.runTool("restore", "-d", dbname2, "--dir", dumppath + dbname, "--noOptionsRestore");

db = db.getSiblingDB(dbname2);

assert.eq(1, db.capped.count(), "wrong number of docs restored to capped");
assert(true !== db.capped.stats().capped, "restore options were not ignored");
assert.eq(defaultFlags,
          db.capped.exists().options,
          "restore options not ignored: " + tojson(db.capped.exists()));

// Dump/restore single collection

db.dropDatabase();
var options = {capped: true, size: 4096, autoIndexId: true};
db.createCollection('capped', options);
assert.eq(1, db.capped.getIndexes().length, "auto index not created");
var cappedOptions = db.capped.exists().options;
for (var opt in options) {
    assert.eq(options[opt], cappedOptions[opt], 'invalid option');
}

assert.writeOK(db.capped.insert({x: 1}));

dumppath = t.ext + "noOptionsSingleColDump/";
mkdir(dumppath);
dbname = db.getName();
t.runTool("dump", "-d", dbname, "-c", "capped", "--out", dumppath);

db.dropDatabase();

assert.eq(0, db.capped.count(), "capped not dropped");
assert.eq(0, db.capped.getIndexes().length, "indexes not dropped");

t.runTool("restore", "-d", dbname, "--drop", "--noOptionsRestore", dumppath + dbname);

db = db.getSiblingDB(dbname);

assert.eq(1, db.capped.count(), "wrong number of docs restored to capped");
assert(true !== db.capped.stats().capped, "restore options were not ignored");
assert.eq(defaultFlags,
          db.capped.exists().options,
          "restore options not ignored: " + tojson(db.capped.exists()));

t.stop();
