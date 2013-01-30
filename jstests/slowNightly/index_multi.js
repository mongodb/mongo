// Attempt to build 63 indexes simultaneously

Random.setRandomSeed();

var coll = db.index_multi;

print("Populate the collection with random data");
for (var i=0;i<1e4; i++) {
    var doc = {"_id" : i};

    for (var j=0; j<100; j++) {
        // Skip some of the fields
        if (Random.rand() < .1) {
            continue;
        }
        // Make 0, 10, etc. multikey indexes
        else if (j % 10 == 0) {
            doc["field"+j] = [Random.rand(), Random.rand(), Random.rand()];
        }
        else {
            doc["field"+j] = Random.rand();
        }
    }

    if (i%1000 == 0) {
        print("inserted "+i);
    }

    coll.insert(doc);
}

// Array of all index specs
var specs = [];
var multikey = [];

print("Create 3 triple indexes");
for (var i=90; i<93; i++) {
    var spec = {};
    spec["field"+i] = 1;
    spec["field"+(i+1)] = 1;
    spec["field"+(i+2)] = 1;
    startParallelShell("db.index_multi.createIndex("+tojson(spec)+", {background:true});"
                       +"db.results.insert(db.runCommand({getlasterror:1}));");
    specs.push(spec);
    multikey.push(i % 10 == 0 || (i+1) % 10 == 0 || (i+2) % 10 == 0);
}

print("Create 30 compound indexes");
for (var i=30; i<90; i+=2) {
    var spec = {};
    spec["field"+i] = 1;
    spec["field"+(i+1)] = 1;
    startParallelShell("db.index_multi.createIndex("+tojson(spec)+", {background:true});"
                       +"db.results.insert(db.runCommand({getlasterror:1}));");
    specs.push(spec);
    multikey.push(i % 10 == 0 || (i+1) % 10 == 0);
}

print("Create 30 indexes");
for (var i=0; i<30; i++) {
    var spec = {};
    spec["field"+i] = 1;
    startParallelShell("db.index_multi.createIndex("+tojson(spec)+", {background:true});"
                       +"db.results.insert(db.runCommand({getlasterror:1}));");
    specs.push(spec);
    multikey.push(i % 10 == 0);
}

print("Do some sets and unsets");
for (i=0; i<1e4; i++) {
    var criteria = {_id: Random.randInt(1e5)};
    var mod = {};
    if (Random.rand() < .5) {
        mod['$set'] = {};
        mod['$set']['field'+Random.randInt(100)] = Random.rand();
    }
    else {
        mod['$unset'] = {};
        mod['$unset']['field'+Random.randInt(100)] = true;
    }

    coll.update(criteria, mod);
}

printjson(db.results.find().toArray());

printjson(coll.getIndexes());

print("Make sure we end up with 64 indexes");
assert.soon(
    function() {
        for (var i in specs) {
            print("trying to hint on "+tojson(specs[i]));
            try {
                var explain = coll.find().hint(specs[i]).explain();
                printjson(explain);
                assert.eq(multikey[i], explain.isMultiKey);
            } catch (x) {
                print(x+", hinting on "+tojson(specs[i]));
                return false;
            }
        }
        return true;
    },
    "wait for all indexes to be built",
    120000
);

print("SUCCESS!");
