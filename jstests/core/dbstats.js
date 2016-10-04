// Confirms that the dbStats command returns expected content.

(function() {
    "use strict";

    function serverIsMongos() {
        const res = db.runCommand("ismaster");
        assert.commandWorked(res);
        return res.msg === "isdbgrid";
    }

    const isMongoS = serverIsMongos();
    const isMMAPv1 = jsTest.options().storageEngine === "mmapv1";

    let testDB = db.getSiblingDB("dbstats_js");
    assert.commandWorked(testDB.dropDatabase());

    let coll = testDB["testColl"];
    assert.commandWorked(coll.createIndex({x: 1}));
    const doc = {_id: 1, x: 1};
    assert.writeOK(coll.insert(doc));

    let dbStats = testDB.runCommand({dbStats: 1});
    assert.commandWorked(dbStats);

    if (isMMAPv1) {
        if (isMongoS) {
            // When this test is run against mongoS with the mmapV1 storage engine the 'objects' and
            // 'indexes' counts will vary depending on whether 'testColl' is sharded and on the # of
            // shards (due to inclusion of system.indexes & system.namespaces counts).
            assert(dbStats.hasOwnProperty("objects"), tojson(dbStats));
            assert(dbStats.hasOwnProperty("indexes"), tojson(dbStats));
        } else {
            assert.eq(7,
                      dbStats.objects,
                      tojson(dbStats));  // Includes testColl, system.indexes & system.namespaces
            assert.eq(2, dbStats.indexes, tojson(dbStats));
        }
        // 'dataSize' and 'avgObjSize' include document padding space under MMAPv1.
        assert(dbStats.hasOwnProperty("dataSize"), tojson(dbStats));
        assert(dbStats.hasOwnProperty("avgObjSize"), tojson(dbStats));
    } else {
        assert.eq(1, dbStats.objects, tojson(dbStats));  // Includes testColl only
        const dataSize = Object.bsonsize(doc);
        assert.eq(dataSize, dbStats.avgObjSize, tojson(dbStats));
        assert.eq(dataSize, dbStats.dataSize, tojson(dbStats));

        // Index count will vary on mongoS if an additional index is needed to support sharding.
        if (isMongoS) {
            assert(dbStats.hasOwnProperty("indexes"), tojson(dbStats));
        } else {
            assert.eq(2, dbStats.indexes, tojson(dbStats));
        }
    }

    assert(dbStats.hasOwnProperty("storageSize"), tojson(dbStats));
    assert(dbStats.hasOwnProperty("numExtents"), tojson(dbStats));
    assert(dbStats.hasOwnProperty("indexSize"), tojson(dbStats));

    // Confirm extentFreeList field existence. Displayed for mongoD running MMAPv1 and for mongoS
    // regardless of storage engine.
    if (isMMAPv1 || isMongoS) {
        assert(dbStats.hasOwnProperty("extentFreeList"), tojson(dbStats));
        assert(dbStats.extentFreeList.hasOwnProperty("num"), tojson(dbStats));
        assert(dbStats.extentFreeList.hasOwnProperty("totalSize"), tojson(dbStats));
    }

    // Confirm collection and view counts on mongoD
    if (!isMongoS) {
        assert.eq(testDB.getName(), dbStats.db, tojson(dbStats));

        // We wait to add a view until this point as it allows more exact testing of avgObjSize for
        // WiredTiger above. Having more than 1 document would require floating point comparison.
        assert.commandWorked(testDB.createView("testView", "testColl", []));

        dbStats = testDB.runCommand({dbStats: 1});
        assert.commandWorked(dbStats);

        if (isMMAPv1) {
            assert.eq(
                4,
                dbStats.collections,
                tojson(dbStats));  // testColl + system.views + system.indexes + system.namespaces
        } else {
            assert.eq(2, dbStats.collections, tojson(dbStats));  // testColl + system.views
        }

        assert.eq(1, dbStats.views, tojson(dbStats));
    }

})();
