/**
 * Test that opcounters are correct for getMore operations on awaitData cursors.
 * @tags: [requires_capped]
 */
(function() {
    "use strict";

    const coll = db.getmore_awaitdata_opcounters;
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: 1024}));
    assert.writeOK(coll.insert({_id: 1}));
    assert.writeOK(coll.insert({_id: 2}));
    assert.writeOK(coll.insert({_id: 3}));

    function getGlobalLatencyStats() {
        return db.serverStatus().opLatencies.reads;
    }

    function getCollectionLatencyStats() {
        return coll.latencyStats().next().latencyStats.reads;
    }

    function getTop() {
        return db.adminCommand({top: 1}).totals[coll.getFullName()];
    }

    // Global latency histogram from serverStatus should record two read ops, one for find and one
    // for getMore.
    let oldGlobalLatency = getGlobalLatencyStats();
    assert.eq(3, coll.find().tailable(true).itcount());
    let newGlobalLatency = getGlobalLatencyStats();
    assert.eq(2, newGlobalLatency.ops - oldGlobalLatency.ops);

    // Per-collection latency histogram should record three read ops, one for find, one for getMore,
    // and one for the aggregation command used to retrieve the stats themselves.
    let oldCollLatency = getCollectionLatencyStats();
    assert.eq(3, coll.find().tailable(true).itcount());
    let newCollLatency = getCollectionLatencyStats();
    assert.eq(3, newCollLatency.ops - oldCollLatency.ops);

    // Top separates counters for getMore and find. We should see a delta of one getMore op and one
    // find op.
    let oldTop = getTop();
    assert.eq(3, coll.find().tailable(true).itcount());
    let newTop = getTop();
    assert.eq(1, newTop.getmore.count - oldTop.getmore.count);
    assert.eq(1, newTop.queries.count - oldTop.queries.count);
}());
