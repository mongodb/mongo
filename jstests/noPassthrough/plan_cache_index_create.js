/**
 * Confirms that the query plan cache is cleared on index build completion, making the newly created
 * index available to queries that had a cached plan prior to the build.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/analyze_plan.js');  // For getCachedPlan().
load("jstests/libs/sbe_util.js");      // For checkSBEEnabled.

const dbName = "test";
const collName = "coll";

// Returns whether there is an active index build.
function indexBuildIsRunning(testDB, indexName) {
    const indexBuildFilter = {
        "command.createIndexes": collName,
        "command.indexes.0.name": indexName,
        "msg": /^Index Build/
    };
    const curOp =
        testDB.getSiblingDB("admin").aggregate([{$currentOp: {}}, {$match: indexBuildFilter}]);
    return curOp.hasNext();
}

// Returns whether a cached plan exists for 'query'.
function assertDoesNotHaveCachedPlan(coll, query) {
    const match = {"createdFromQuery.query": query};
    assert.eq([], coll.getPlanCache().list([{$match: match}]), coll.getPlanCache().list());
}

// Returns the cached plan for 'query'.
function getIndexNameForCachedPlan(coll, query) {
    const match = {"createdFromQuery.query": query};
    const plans = coll.getPlanCache().list([{$match: match}]);
    assert.eq(plans.length, 1, coll.getPlanCache().list());
    assert(plans[0].hasOwnProperty("cachedPlan"), plans);

    const cachedPlan = getCachedPlan(plans[0].cachedPlan);
    assert(cachedPlan.hasOwnProperty("inputStage"), plans);
    assert(cachedPlan.inputStage.hasOwnProperty("indexName"), plans);

    return cachedPlan.inputStage.indexName;
}

function runTest({rst, readDB, writeDB}) {
    const readColl = readDB.getCollection(collName);
    const writeColl = writeDB.getCollection(collName);

    assert.commandWorked(writeDB.runCommand({dropDatabase: 1, writeConcern: {w: "majority"}}));

    const bulk = writeColl.initializeUnorderedBulkOp();
    for (let i = 0; i < 100; ++i) {
        bulk.insert({x: i, y: i % 10, z: 0});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));
    // We start with a baseline of 2 existing indexes as we will not cache plans when only a
    // single plan exists.
    assert.commandWorked(writeDB.runCommand({
        createIndexes: collName,
        indexes: [
            {key: {y: 1}, name: "less_selective", background: false},
            {key: {z: 1}, name: "least_selective", background: false}
        ],
        writeConcern: {w: "majority"}
    }));

    rst.awaitReplication();

    //
    // Confirm that the plan cache is reset on start and completion of a background index build.
    //

    // Execute a find and confirm that a cached plan exists for an existing index.
    const filter = {x: 50, y: 0, z: 0};
    assert.eq(readColl.find(filter).itcount(), 1);
    assert.eq("less_selective", getIndexNameForCachedPlan(readColl, filter));

    // Enable a failpoint that will cause an index build to block just after start. This will
    // allow us to examine PlanCache contents while index creation is in flight.
    assert.commandWorked(
        readDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'}));

    // The commitIndexBuild oplog entry may block $planCacheStats on the secondary during oplog
    // application because it will hold the PBWM while waiting for the index build to complete in
    // the backgroud. Therefore, we get the primary to hold off on writing the commitIndexBuild
    // oplog entry until we are ready to resume index builds on the secondary.
    if (writeDB.getMongo().host != readDB.getMongo().host) {
        assert.commandWorked(writeDB.adminCommand(
            {configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'}));
    }

    // Build a "most selective" index in the background.
    TestData.dbName = dbName;
    TestData.collName = collName;
    const createIdxShell = startParallelShell(function() {
        const testDB = db.getSiblingDB(TestData.dbName);
        assert.commandWorked(testDB.runCommand({
            createIndexes: TestData.collName,
            indexes: [{key: {x: 1}, name: "most_selective", background: true}],
            writeConcern: {w: "majority"}
        }));
    }, writeDB.getMongo().port);

    // Confirm that the index build has started.
    assert.soon(() => indexBuildIsRunning(readDB, "most_selective"),
                "Index build operation not found after starting via parallelShell");

    // Confirm that there are no cached plans post index build start.
    assertDoesNotHaveCachedPlan(readColl, filter);

    // Execute a find and confirm that a previously built index is the cached plan.
    assert.eq(readColl.find(filter).itcount(), 1);
    assert.eq("less_selective", getIndexNameForCachedPlan(readColl, filter));

    // Disable the hang and wait for the index build to complete.
    assert.commandWorked(
        readDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'}));

    // No effect if the fail point is already disabled.
    assert.commandWorked(
        writeDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'}));

    assert.soon(() => !indexBuildIsRunning(readDB, "most_selective"));
    createIdxShell({checkExitSuccess: true});

    rst.awaitReplication();

    // Confirm that there are no cached plans post index build.
    assertDoesNotHaveCachedPlan(readColl, filter);

    // Now that the index has been built, execute another find and confirm that the newly
    // created index is used.
    assert.eq(readColl.find(filter).itcount(), 1);
    assert.eq("most_selective", getIndexNameForCachedPlan(readColl, filter));

    // Drop the newly created index and confirm that the plan cache has been cleared.
    assert.commandWorked(
        writeDB.runCommand({dropIndexes: collName, index: {x: 1}, writeConcern: {w: "majority"}}));
    assertDoesNotHaveCachedPlan(readColl, filter);

    //
    // Confirm that the plan cache is reset post foreground index build.
    //

    // Execute a find and confirm that an existing index is in the cache.
    assert.eq(readColl.find(filter).itcount(), 1);
    assert.eq("less_selective", getIndexNameForCachedPlan(readColl, filter));

    // Build a "most selective" index in the foreground.
    assert.commandWorked(writeDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {x: 1}, name: "most_selective", background: false}],
        writeConcern: {w: "majority"}
    }));

    rst.awaitReplication();

    // Confirm that there are no cached plans post index build.
    assertDoesNotHaveCachedPlan(readColl, filter);

    // Execute a find and confirm that the newly created index is used.
    assert.eq(readColl.find(filter).itcount(), 1);
    assert.eq("most_selective", getIndexNameForCachedPlan(readColl, filter));

    // Drop the newly created index and confirm that the plan cache has been cleared.
    assert.commandWorked(
        writeDB.runCommand({dropIndexes: collName, index: {x: 1}, writeConcern: {w: "majority"}}));
    assertDoesNotHaveCachedPlan(readColl, filter);
}

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const primaryDB = rst.getPrimary().getDB(dbName);
const secondaryDB = rst.getSecondary().getDB(dbName);

if (checkSBEEnabled(primaryDB, ["featureFlagSbeFull"])) {
    jsTest.log("Skipping test because SBE is fully enabled");
    rst.stopSet();
    return;
}

runTest({rst: rst, readDB: primaryDB, writeDB: primaryDB});
runTest({rst: rst, readDB: secondaryDB, writeDB: primaryDB});

rst.stopSet();
})();
