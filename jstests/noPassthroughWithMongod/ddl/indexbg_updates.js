// Perform concurrent updates with a background index build, testing that
// indexes are updated appropriately. See SERVER-23590 and SERVER-22970.
//
// Variation of index_multi.js

Random.setRandomSeed();

let coll = db.getSiblingDB("indexbg_updates").coll;
coll.drop();

let numDocs = 10000;

let bulk = coll.initializeUnorderedBulkOp();
print("Populate the collection with random data");
for (var i = 0; i < numDocs; i++) {
    let doc = {"_id": i, "field0": Random.rand()};

    bulk.insert(doc);
}
assert.commandWorked(bulk.execute());

// Perform a bulk update on a single document, targeting the updates on the
// field being actively indexed in the background
bulk = coll.initializeUnorderedBulkOp();
for (i = 0; i < numDocs; i++) {
    let criteria = {"_id": 1000};
    let mod = {};

    if (Random.rand() < 0.8) {
        mod["$set"] = {};
        mod["$set"]["field0"] = Random.rand();
    } else {
        mod["$unset"] = {};
        mod["$unset"]["field0"] = true;
    }

    bulk.find(criteria).update(mod);
}

// Build an index in the background on field0
let backgroundIndexBuildShell = startParallelShell(
    function () {
        let coll = db.getSiblingDB("indexbg_updates").coll;
        assert.commandWorked(coll.createIndex({"field0": 1}, {"background": true}));
    },
    null, // port -- use default
    false, // noconnect
);

print("Do some sets and unsets");
assert.commandWorked(bulk.execute());

print("Start background index build");
backgroundIndexBuildShell();

let explain = coll.find().hint({"field0": 1}).explain();
assert("queryPlanner" in explain, tojson(explain));
