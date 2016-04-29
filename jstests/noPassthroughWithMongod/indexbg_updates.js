// Perform concurrent updates with a background index build, testing that
// indexes are updated appropriately. See SERVER-23590 and SERVER-22970.
//
// This test is a variation of index_multi.js.

(function() {
    "use strict";
    Random.setRandomSeed();

    var coll = db.getSiblingDB("indexbg_updates").coll;
    coll.drop();

    var numDocs = 10000;

    var bulk = coll.initializeUnorderedBulkOp();
    print("Populate the collection with random data");
    for (var i = 0; i < numDocs; i++) {
        var doc = {
            "_id": i,
            "field0": Random.rand()
        };

        bulk.insert(doc);
    }
    assert.writeOK(bulk.execute());

    // Perform a bulk update on a single document, targeting the updates on the
    // field being actively indexed in the background.
    bulk = coll.initializeUnorderedBulkOp();
    for (i = 0; i < numDocs; i++) {
        var criteria = {
            "_id": 1000
        };
        var mod = {};

        if (Random.rand() < .8) {
            mod["$set"] = {};
            mod["$set"]["field0"] = Random.rand();
        } else {
            mod["$unset"] = {};
            mod["$unset"]["field0"] = true;
        }

        bulk.find(criteria).update(mod);
    }

    // Build an index in the background on field0.
    var backgroundIndexBuildShell = startParallelShell(
        function() {
            var coll = db.getSiblingDB("indexbg_updates").coll;
            assert.commandWorked(coll.createIndex({"field0": 1}, {"background": true}));
        },
        null,  // port -- use default
        false  // noconnect
        );

    print("Do some sets and unsets");
    assert.writeOK(bulk.execute());

    print("Start background index build");
    backgroundIndexBuildShell();

    var explain = coll.find().hint({"field0": 1}).explain();
    assert("queryPlanner" in explain, tojson(explain));

    // Validate the collection.
    // In MongoDB 3.0, when a non-multi-key index has a mismatching number of keys and documents,
    // validate does not produce an error. Therefore, we do a manual check of the number of keys.
    var validationOutput = coll.validate(true);
    printjson(validationOutput);

    var indexes = Object.keys(validationOutput["keysPerIndex"]);
    assert.eq(indexes.length, 2, "Expected only two indexes but found " + indexes);

    for (var i = 0; i < indexes.length; i++) {
        var keys = validationOutput["keysPerIndex"][indexes[i]];
        assert.eq(keys, numDocs,
                  "Regular index " + indexes[i] + " has a mismatching number of keys (" + keys +
                  ") with documents in collection (" + numDocs + ")");
    }
}());
