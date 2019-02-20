/**
 * Test oplog visibility enforcement of primaries and secondaries. This test uses a client to read
 * the oplog while there are concurrent writers. The client copies all the timestamps it sees and
 * verifies a later scan over the range returns the same values.
 *
 * @tags: [requires_document_locking]
 */
(function() {
    "use strict";

    load("jstests/libs/parallelTester.js");  // for ScopedThread.

    const replTest = new ReplSetTest({
        name: "oplog_visibility",
        nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        settings: {chainingAllowed: true}
    });
    replTest.startSet();
    replTest.initiate();

    jsTestLog("Enabling `sleepBeforeCommit` failpoint.");
    for (let node of replTest.nodes) {
        assert.commandWorked(node.adminCommand(
            {configureFailPoint: "sleepBeforeCommit", mode: {activationProbability: 0.01}}));
    }

    jsTestLog("Starting concurrent writers.");
    let stopLatch = new CountDownLatch(1);
    let writers = [];
    for (let idx = 0; idx < 2; ++idx) {
        let coll = "coll_" + idx;
        let writer = new ScopedThread(function(host, coll, stopLatch) {
            const conn = new Mongo(host);
            let id = 0;

            // Cap the amount of data being inserted to avoid rolling over a 10MiB oplog. It takes
            // ~70,000 "basic" ~150 byte oplog documents to fill a 10MiB oplog. Note this number is
            // for each of two writer threads.
            const maxDocsToInsert = 20 * 1000;
            while (stopLatch.getCount() > 0 && id < maxDocsToInsert) {
                conn.getDB("test").getCollection(coll).insert({_id: id});
                id++;
            }
            jsTestLog({"NumDocsWritten": id});
        }, replTest.getPrimary().host, coll, stopLatch);

        writer.start();
        writers.push(writer);
    }

    for (let node of replTest.nodes) {
        let testOplog = function(node) {
            let timestamps = [];

            let local = node.getDB("local");
            let oplogStart =
                local.getCollection("oplog.rs").find().sort({$natural: -1}).limit(-1).next()["ts"];
            jsTestLog({"Node": node.host, "StartTs": oplogStart});

            while (timestamps.length < 1000) {
                // Query with $gte to validate continuinity. Do not add this first record to the
                // recorded timestamps. Its value was already added in the last cursor.
                let cursor = local.getCollection("oplog.rs")
                                 .find({ts: {$gte: oplogStart}})
                                 .sort({$natural: 1})
                                 .tailable(true)
                                 .batchSize(100);
                assert(cursor.hasNext());
                assert.eq(oplogStart, cursor.next()["ts"]);

                // While this method wants to capture 1000 timestamps, the cursor has a batch size
                // of 100 and this loop makes 200 iterations before getting a new cursor from a
                // fresh query. The goal is to exercise getMores, which use different code paths
                // for establishing their oplog reader transactions.
                for (let num = 0; num < 200 && timestamps.length < 1000; ++num) {
                    try {
                        if (cursor.hasNext() == false) {
                            break;
                        }
                    } catch (exc) {
                        break;
                    }
                    let ts = cursor.next()["ts"];
                    timestamps.push(ts);
                    oplogStart = ts;
                }
            }

            jsTestLog({"Verifying": node.host, "StartTs": timestamps[0], "EndTs": timestamps[999]});
            oplogStart = timestamps[0];
            let cursor =
                local.getCollection("oplog.rs").find({ts: {$gte: oplogStart}}).sort({$natural: 1});
            for (let observedTsIdx in timestamps) {
                let observedTs = timestamps[observedTsIdx];
                assert(cursor.hasNext());
                let actualTs = cursor.next()["ts"];
                assert.eq(actualTs, observedTs, function() {
                    let prev = null;
                    let next = null;
                    if (observedTsIdx > 0) {
                        prev = timestamps[observedTsIdx - 1];
                    }
                    if (observedTsIdx + 1 < timestamps.length) {
                        next = timestamps[observedTsIdx + 1];
                    }

                    return tojson({
                        "Missing": actualTs,
                        "ObservedIdx": observedTsIdx,
                        "PrevObserved": prev,
                        "NextObserved": next
                    });
                });
            }
        };

        jsTestLog({"Testing": node.host});
        testOplog(node);
    }
    jsTestLog("Stopping writers.");
    stopLatch.countDown();
    writers.forEach((writer) => {
        writer.join();
    });

    replTest.stopSet();
})();
