// Tests the high-water memory fragmentation level incurred by the allocator
// while running on WiredTiger. The maximum allowed fragmentation should be
// limited to a percentage of the WiredTiger cache size.

(function() {
    "use strict";

    var jobs = [];
    var numUpdaterJobs = 16;

    // Initialize collections
    var dataCollection = db.getSiblingDB("malloc_frag").data;
    var resultsCollection = db.getSiblingDB("malloc_frag").completed;
    var statsCollection = db.getSiblingDB("malloc_frag").statistics;
    dataCollection.drop();
    resultsCollection.drop();
    statsCollection.drop();

    // Acceptable fragmentation is 10% of wiredTigerCacheSizeGB
    var wiredTigerCacheSize = db.serverStatus().wiredTiger.cache["maximum bytes configured"];
    var maxFragmentation = 0.1 * wiredTigerCacheSize;
    var targetDocumentSize = 16.0 * 1024.0;
    var numDocs = Math.floor(wiredTigerCacheSize / targetDocumentSize);

    print("Populate collection with data");
    var bulk = dataCollection.initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        var doc = {
            _id: i,
            data: "x"
        };

        bulk.insert(doc);
    }
    assert.writeOK(bulk.execute());

    // Poll for fragmentation statistics via serverStatus
    var pollServerStatusShell = startParallelShell(
        function() {
            var resultsCollection = db.getSiblingDB("malloc_frag").completed;
            var statsCollection = db.getSiblingDB("malloc_frag").statistics;

            while (resultsCollection.count() < 16) {
                var s = db.runCommand({
                    serverStatus: 1,
                    tcmalloc: 1,
                    asserts: 0,
                    backgroundFlushing: 0,
                    dur: 0,
                    extra_info: 0,
                    globalLock: 0,
                    locks: 0,
                    mem: 0,
                    metrics: 0,
                    network: 0,
                    opcounters: 0,
                    opcountersRepl: 0,
                    rangeDeleter: 0,
                    repl: 0,
                    security: 0,
                    storageEngine: 0,
                    wiredTiger: 0,
                    writeBacksQueued: 0,
                    connections: 0
                });
                statsCollection.insert({
                    localTime: s.localTime,
                    totalFreeBytes: s.tcmalloc.tcmalloc.total_free_bytes
                });
                sleep(1000);
            }
        },
        null,  // port -- use default
        false  // noconnect
        );
    jobs.push(pollServerStatusShell);

    // Slowly perform growing updates on documents
    for (var i = 0; i < numUpdaterJobs; i++) {
        jobs.push(startParallelShell(
            function() {
                Random.setRandomSeed();

                var wtCacheSize = db.serverStatus().wiredTiger.cache["maximum bytes configured"];
                var targetDocumentSize = 16.0 * 1024.0;
                var numDocs = Math.floor(wtCacheSize / targetDocumentSize);
                var dataCollection = db.getSiblingDB("malloc_frag").data;
                var resultsCollection = db.getSiblingDB("malloc_frag").completed;

                for (var i = 0; i < 20000; i++) {
                    var data = "x".repeat(Random.randInt(200));
                    var id = Random.randInt(numDocs);
                    dataCollection.update({_id: id}, {data: data});
                }
                resultsCollection.insert({complete: 1});
            },
            null,  // port -- use default
            false  // noconnect
            ));
    }

    jobs.forEach(function(join) {
        join();
    });

    // What was the highest level of fragmentation seen during the test?
    var result =
        statsCollection.aggregate([{"$sort": {"totalFreeBytes": 1}}, {"$limit": 1}]).toArray();
    var actualFragmentation = result[0]["totalFreeBytes"];

    // Clean up now, in case the assertion fails
    db.getSiblingDB("malloc_frag").dropDatabase();

    print("WiredTiger cache size:          " + wiredTigerCacheSize + " bytes");
    print("Highest observed fragmentation: " + actualFragmentation + " bytes");
    assert(actualFragmentation < maxFragmentation,
           "Memory fragmentation of " + actualFragmentation + " bytes exceeded maximum allowable " +
               "fragmentation of " + maxFragmentation + " bytes");
}());
