// Test that top and latency histogram statistics are recorded for views.

(function() {
    "use strict";
    load("jstests/libs/stats.js");

    let viewsDB = db.getSiblingDB("views_stats");
    assert.commandWorked(viewsDB.dropDatabase());
    assert.commandWorked(viewsDB.runCommand({create: "view", viewOn: "collection"}));

    let view = viewsDB["view"];
    let coll = viewsDB["collection"];

    // Check the histogram counters.
    let lastHistogram = getHistogramStats(view);
    view.aggregate([{$match: {}}]);
    lastHistogram = assertHistogramDiffEq(view, lastHistogram, 1, 0, 0);

    // Check that failed inserts, updates, and deletes are counted.
    assert.writeError(view.insert({}));
    lastHistogram = assertHistogramDiffEq(view, lastHistogram, 0, 1, 0);

    assert.writeError(view.remove({}));
    lastHistogram = assertHistogramDiffEq(view, lastHistogram, 0, 1, 0);

    assert.writeError(view.update({}, {}));
    lastHistogram = assertHistogramDiffEq(view, lastHistogram, 0, 1, 0);

    let isMasterResponse = assert.commandWorked(viewsDB.runCommand("isMaster"));
    const isMongos = (isMasterResponse.msg === "isdbgrid");
    if (isMongos) {
        jsTest.log("Tests are being run on a mongos; skipping top tests.");
        return;
    }

    // Check the top counters.
    let lastTop = getTop(view);
    view.aggregate([{$match: {}}]);
    lastTop = assertTopDiffEq(view, lastTop, "commands", 1);

    assert.writeError(view.insert({}));
    lastTop = assertTopDiffEq(view, lastTop, "insert", 1);

    assert.writeError(view.remove({}));
    lastTop = assertTopDiffEq(view, lastTop, "remove", 1);

    assert.writeError(view.update({}, {}));
    lastTop = assertTopDiffEq(view, lastTop, "update", 1);

    // Check that operations on the backing collection do not modify the view stats.
    lastTop = getTop(view);
    lastHistogram = getHistogramStats(view);
    assert.writeOK(coll.insert({}));
    assert.writeOK(coll.update({}, {$set: {x: 1}}));
    coll.aggregate([{$match: {}}]);
    assert.writeOK(coll.remove({}));

    assertTopDiffEq(view, lastTop, "insert", 0);
    assertTopDiffEq(view, lastTop, "update", 0);
    assertTopDiffEq(view, lastTop, "remove", 0);
    assertHistogramDiffEq(view, lastHistogram, 0, 0, 0);
}());
