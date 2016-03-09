/**
 * Test that a TTL index on a capped collection doesn't crash the server or cause the TTL monitor
 * to skip processing other (non-capped) collections on the database.
 */
(function() {
    "use strict";

    var dbpath = MongoRunner.dataPath + "ttl_capped";
    resetDbpath(dbpath);

    var conn = MongoRunner.runMongod({
        dbpath: dbpath,
        noCleanData: true,
        setParameter: "ttlMonitorSleepSecs=1",
    });
    assert.neq(null, conn, "mongod was unable to start up");

    var testDB = conn.getDB("test");

    assert.commandWorked(testDB.adminCommand({setParameter: 1, ttlMonitorEnabled: false}));

    var now = Date.now();
    var expireAfterSeconds = 10;

    var numCollectionsToCreate = 20;
    var width = numCollectionsToCreate.toString().length;

    // Create 'numCollectionsToCreate' collections with a TTL index, where every third collection is
    // capped. We create many collections with a TTL index to increase the odds that the TTL monitor
    // would process a non-capped collection after a capped collection. This allows us to verify
    // that the TTL monitor continues processing the remaining collections after encountering an
    // error processing a capped collection.
    for (var i = 0; i < numCollectionsToCreate; i++) {
        var collName = "ttl" + i.zeroPad(width);
        if (i % 3 === 1) {
            assert.commandWorked(testDB.createCollection(collName, {capped: true, size: 4096}));
        }

        // Create a TTL index on the 'date' field of the collection.
        var res = testDB[collName].ensureIndex({date: 1}, {expireAfterSeconds: expireAfterSeconds});
        assert.commandWorked(res);

        // Insert a single document with a 'date' field that is already expired according to the
        // index definition.
        assert.writeOK(testDB[collName].insert({date: new Date(now - expireAfterSeconds * 1000)}));
    }

    // Increase the verbosity of the TTL monitor's output.
    assert.commandWorked(testDB.adminCommand({setParameter: 1, logComponentVerbosity: {index: 1}}));

    // Enable the TTL monitor and wait for it to run.
    var ttlPasses = testDB.serverStatus().metrics.ttl.passes;
    assert.commandWorked(testDB.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));

    var timeoutSeconds = 5;
    assert.soon(
        function checkIfTTLMonitorRan() {
            // The 'ttl.passes' metric is incremented when the TTL monitor starts processing the
            // indexes, so we wait for it to be incremented twice to know that the TTL monitor
            // finished processing the indexes at least once.
            return testDB.serverStatus().metrics.ttl.passes >= ttlPasses + 2;
        },
        function msg() {
            return "TTL monitor didn't run within " + timeoutSeconds + " seconds";
        },
        timeoutSeconds * 1000);

    for (var i = 0; i < numCollectionsToCreate; i++) {
        var coll = testDB["ttl" + i.zeroPad(width)];
        var count = coll.count();
        if (i % 3 === 1) {
            assert.eq(1,
                      count,
                      "the TTL monitor shouldn't have removed expired documents from" +
                          " the capped collection '" + coll.getFullName() + "'");
        } else {
            assert.eq(0,
                      count,
                      "the TTL monitor didn't removed expired documents from the" +
                          " collection '" + coll.getFullName() + "'");
        }
    }

    MongoRunner.stopMongod(conn);
})();
