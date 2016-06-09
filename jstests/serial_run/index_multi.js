// Attempt to build 63 indexes simultaneously

Random.setRandomSeed();

var coll = db.index_multi;
coll.drop();
db.results.drop();

var bulk = coll.initializeUnorderedBulkOp();
print("Populate the collection with random data");
for (var i = 0; i < 1e4; i++) {
    var doc = {"_id": i};

    for (var j = 0; j < 100; j++) {
        // Skip some of the fields
        if (Random.rand() < .1) {
            continue;
        }
        // Make 0, 10, etc. multikey indexes
        else if (j % 10 == 0) {
            doc["field" + j] = [Random.rand(), Random.rand(), Random.rand()];
        } else {
            doc["field" + j] = Random.rand();
        }
    }

    bulk.insert(doc);
}
assert.writeOK(bulk.execute());

// Array of all index specs
var specs = [];
var multikey = [];

var setupDBStr = "var conn = null;" + "assert.soon(function() {" + "  try {" +
    "    conn = new Mongo(\"" + db.getMongo().host + "\");" + "    return conn;" +
    "  } catch (x) {" + "    return false;" + "  }" +
    "}, 'Timed out waiting for temporary connection to connect', 30000, 5000);" +
    "var db = conn.getDB('" + db.getName() + "');";

var indexJobs = [];
print("Create 3 triple indexes");
for (var i = 90; i < 93; i++) {
    var spec = {};
    spec["field" + i] = 1;
    spec["field" + (i + 1)] = 1;
    spec["field" + (i + 2)] = 1;
    indexJobs.push(startParallelShell(
        setupDBStr + "printjson(db.index_multi.createIndex(" + tojson(spec) + "," +
            "{ background: true }));" + "db.results.insert(Object.extend(" +
            "db.runCommand({ getlasterror: 1 }), " + tojson(spec) + ") );",
        null,    // port
        true));  // noconnect
    specs.push(spec);
    multikey.push(i % 10 == 0 || (i + 1) % 10 == 0 || (i + 2) % 10 == 0);
}

print("Create 30 compound indexes");
for (var i = 30; i < 90; i += 2) {
    var spec = {};
    spec["field" + i] = 1;
    spec["field" + (i + 1)] = 1;
    indexJobs.push(startParallelShell(
        setupDBStr + "printjson(db.index_multi.createIndex(" + tojson(spec) + ", " +
            "{ background: true }));" + "db.results.insert(Object.extend(" +
            "db.runCommand({ getlasterror: 1 }), " + tojson(spec) + ") );",
        null,    // port
        true));  // noconnect
    specs.push(spec);
    multikey.push(i % 10 == 0 || (i + 1) % 10 == 0);
}

print("Create 30 indexes");
for (var i = 0; i < 30; i++) {
    var spec = {};
    spec["field" + i] = 1;
    indexJobs.push(startParallelShell(
        setupDBStr + "printjson(db.index_multi.createIndex(" + tojson(spec) + ", " +
            "{ background: true }));" + "db.results.insert(Object.extend(" +
            "db.runCommand({ getlasterror: 1 }), " + tojson(spec) + ") );",
        null,    // port
        true));  // noconnect
    specs.push(spec);
    multikey.push(i % 10 == 0);
}

print("Do some sets and unsets");
bulk = coll.initializeUnorderedBulkOp();
for (i = 0; i < 1e4; i++) {
    var criteria = {_id: Random.randInt(1e5)};
    var mod = {};
    if (Random.rand() < .5) {
        mod['$set'] = {};
        mod['$set']['field' + Random.randInt(100)] = Random.rand();
    } else {
        mod['$unset'] = {};
        mod['$unset']['field' + Random.randInt(100)] = true;
    }

    bulk.find(criteria).update(mod);
}
assert.writeOK(bulk.execute());

indexJobs.forEach(function(join) {
    join();
});

printjson(db.results.find().toArray());
// assert.eq(coll.getIndexes().length, 64, "didn't see 64 indexes");

print("Make sure we end up with 64 indexes");
for (var i in specs) {
    var explain = coll.find().hint(specs[i]).explain();
    assert("queryPlanner" in explain, tojson(explain));
}

print("SUCCESS!");
