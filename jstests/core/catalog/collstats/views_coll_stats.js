/**
 * Test that $collStats works on a view and in view pipelines as expected.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   requires_collstats,
 * ]
 */

let viewsDB = db.getSiblingDB("views_coll_stats");
const matchStage = {
    $match: {},
};
const collStatsStage = {
    $collStats: {latencyStats: {}},
};

function clear() {
    assert.commandWorked(viewsDB.dropDatabase());
}

function getCollStats(ns) {
    return viewsDB[ns].latencyStats().next();
}

function checkCollStatsBelongTo(stats, expectedNs) {
    assert.eq(
        stats.ns,
        viewsDB[expectedNs].getFullName(),
        "Expected coll stats for " + expectedNs + " but got " + stats.ns,
    );
}

function makeView(viewNs, viewOnNs, pipeline) {
    if (!pipeline) {
        pipeline = [];
    }
    let res = viewsDB.runCommand({create: viewNs, viewOn: viewOnNs, pipeline: pipeline});
    assert.commandWorked(res);
}

clear();

// Check basic latency stats on a view.
makeView("a", "b");
checkCollStatsBelongTo(viewsDB["a"].latencyStats().next(), "a");
clear();

// Check that latency stats does not prepend the view pipeline.
makeView("a", "b", [matchStage]);
checkCollStatsBelongTo(viewsDB["a"].latencyStats().next(), "a");
clear();

// Check that latency stats works inside a pipeline.
makeView("a", "b", [collStatsStage]);
checkCollStatsBelongTo(viewsDB["a"].latencyStats().next(), "a");
checkCollStatsBelongTo(viewsDB["b"].latencyStats().next(), "b");
// Since the $collStats stage is in the pipeline, it should refer to the viewOn namespace.
checkCollStatsBelongTo(viewsDB["a"].aggregate().next(), "b");
clear();

// Check that the first $collStats pipeline stage found will not resolve further views.
makeView("a", "b", [collStatsStage, matchStage]);
makeView("b", "c", [collStatsStage]);
checkCollStatsBelongTo(viewsDB["a"].latencyStats().next(), "a");
checkCollStatsBelongTo(viewsDB["b"].latencyStats().next(), "b");
checkCollStatsBelongTo(viewsDB["c"].latencyStats().next(), "c");
checkCollStatsBelongTo(viewsDB["a"].aggregate().next(), "b");
checkCollStatsBelongTo(viewsDB["b"].aggregate().next(), "c");
clear();

// Assert that attempting to retrieve storageStats fails.
makeView("a", "b");
assert.commandFailedWithCode(
    viewsDB.runCommand({aggregate: "a", pipeline: [{$collStats: {storageStats: {}}}], cursor: {}}),
    ErrorCodes.CommandNotSupportedOnView,
);
clear();

// Assert that attempting to retrieve collection record count on an identity views fails.
makeView("a", "b");
assert.commandFailedWithCode(
    viewsDB.runCommand({aggregate: "a", pipeline: [{$collStats: {count: {}}}], cursor: {}}),
    ErrorCodes.CommandNotSupportedOnView,
);
clear();

// Assert that attempting to retrieve collection record count on a non-identity view fails.
makeView("a", "b", [{$match: {a: 0}}]);
assert.commandFailedWithCode(
    viewsDB.runCommand({aggregate: "a", pipeline: [{$collStats: {count: {}}}], cursor: {}}),
    ErrorCodes.CommandNotSupportedOnView,
);
clear();
