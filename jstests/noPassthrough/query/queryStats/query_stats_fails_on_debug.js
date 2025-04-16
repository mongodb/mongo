/**
 * Test that query stats errors fail the user query only when debug build is enabled or
 * internalQueryStatsErrorsAreCommandFatal is true. The user query should succeed otherwise.
 */

import {
    withQueryStatsEnabled,
} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();
const queryKnobName = "internalQueryStatsErrorsAreCommandFatal";

function setUpTest(db, areErrorsFatal, failPoint, isDebugBuild) {
    jsTest.log.info('Running test with params isDebugBuild: ' + isDebugBuild +
                    ', areErrorsFatal: ' + areErrorsFatal + ', failpoint: ' + failPoint);

    const queryKnobValue = assert.commandWorked(db.adminCommand(
        {getParameter: 1, internalQueryStatsErrorsAreCommandFatal: 1}))[queryKnobName];
    assert.eq(
        queryKnobValue,
        areErrorsFatal,
        'Expected and actual value of internalQueryStatsErrorsAreCommandFatal are different. Expected: ' +
            areErrorsFatal + ', Actual: ' + queryKnobValue);

    assert.commandWorked(db.adminCommand({'configureFailPoint': failPoint, 'mode': 'alwaysOn'}));
};

// queryStats::registerRequest should only fail the user request in debug builds or
// if internalQueryStatsErrorsAreCommandFatal is true. The user request should succeed otherwise.
function testQueryStatsRegisterRequest(db, coll, areErrorsFatal) {
    const isDebugBuild = db.adminCommand('buildInfo').debug;
    const failPoint = "queryStatsFailToSerializeKey";
    setUpTest(db, areErrorsFatal, failPoint, isDebugBuild);

    if (isDebugBuild || areErrorsFatal) {
        assert.commandFailedWithCode(
            db.runCommand({aggregate: coll.getName(), cursor: {}, pipeline: [{$count: "count"}]}),
            ErrorCodes.QueryStatsFailedToRecord);
    } else {
        assert.eq(coll.aggregate([{$count: "count"}]).toArray(), [{"count": 1}]);
    }
    assert.commandWorked(db.adminCommand({configureFailPoint: failPoint, mode: "off"}));
};

// $queryStats should fail in debug builds or if internalQueryStatsErrorsAreCommandFatal is true,
// and succeed otherwise.
function testQueryStatsAggStage(db, coll, areErrorsFatal) {
    const isDebugBuild = db.adminCommand('buildInfo').debug;
    const failPoint = "queryStatsFailToReparseQueryShape";
    setUpTest(db, areErrorsFatal, failPoint, isDebugBuild);

    // Run a query so the query stats store isn't empty.
    assert.eq(coll.aggregate([{$count: "count"}]).toArray(), [{"count": 1}]);

    const command = {aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}};
    if (isDebugBuild || areErrorsFatal) {
        assert.commandFailedWithCode(db.adminCommand(command), ErrorCodes.QueryStatsFailedToRecord);
    } else {
        assert.commandWorked(db.adminCommand(command));
    }
    assert.commandWorked(db.adminCommand({configureFailPoint: failPoint, mode: "off"}));
};

// Run on sharded clusters and standalone.
withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();
    const adminDB = testDB.getSiblingDB("admin");
    coll.drop();
    coll.insert({v: 1});

    [true, false].forEach(areErrorsFatal => {
        assert.commandWorked(adminDB.runCommand(
            {setParameter: 1, internalQueryStatsErrorsAreCommandFatal: areErrorsFatal}));
        testQueryStatsRegisterRequest(testDB, coll, areErrorsFatal);
        testQueryStatsAggStage(testDB, coll, areErrorsFatal);
    });
});
