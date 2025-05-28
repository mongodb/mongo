/**
 * Validate that enabling the profiler with a filter that depends on new query features can be
 * parsed and used on the latest FCV. When the FCV is downgraded, the filter should be
 * persisted and not error when the profiler is run. However, making a new filter with new query
 * features on the downgraded FCV should error.
 *
 * This test validates the profile and setProfilingFilterGlobally command.
 */

import {findMatchingLogLine} from "jstests/libs/log.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// TODO SERVER-104457 Add a permanent test for this scenario.
const collName = jsTestName();
const pipeline = [{$count: "count"}];

// $convert numeric is guarded by 'featureFlagBinDataConvertNumeric' under FCV 8.1.
const filter = {
    $expr: {$gte: [{$convert: {input: "$docsExamined", to: "binData", byteOrder: "big"}}, true]}
};
const parsedFilter = {
    $expr: {
        $gte: [
            {
                $convert:
                    {input: "$docsExamined", to: {$const: "binData"}, byteOrder: {$const: "big"}},
            },
            {$const: true}
        ]
    }
};

function runAggregationAndDowngradeFCV(coll, adminDB) {
    // Run an aggregation pipeline and assert it worked. The profiler is running in the background.
    // Other tests verify the correctness of the filter, we will just validate the profiler does not
    // cause the aggregation to error.
    assert.eq(coll.aggregate(pipeline).toArray(), [{"count": 1}]);

    // Downgrade the feature compatibility version to 8.0.
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    checkFCV(adminDB, lastLTSFCV);

    // Run the pipeline again. The profile filter is persisted, which is validated later in the
    // test, and will work in the background.
    assert.eq(coll.aggregate(pipeline).toArray(), [{"count": 1}]);
};

function testProfileCommand(conn, isMongos) {
    const profileLevel = isMongos ? 0 : 1;
    const testDB = conn.getDB(jsTestName());
    const adminDB = conn.getDB("admin");
    const coll = testDB[collName];
    assert(coll.drop());
    assert.commandWorked(coll.insert({a: 1, x: "hello"}));

    assert.commandWorked(testDB.setProfilingLevel(profileLevel, {filter: filter}));

    runAggregationAndDowngradeFCV(coll, adminDB);

    // Change the profiler and check the response to show the filter was persisted from the FCV
    // downgrade.
    const response = assert.commandWorked(testDB.setProfilingLevel(0));
    assert.eq(response.filter, parsedFilter);

    if (isMongos) {
        // mongos does not sync FCV with the shards. The FCV on mongos will not be downgraded and
        // the filter should succeed to parse.
        assert.commandWorked(testDB.runCommand({profile: profileLevel, filter: filter}));
    } else {
        // We should not be able to make a profiler entry with the filter on the downgraded FCV.
        assert.commandFailedWithCode(testDB.runCommand({profile: profileLevel, filter: filter}),
                                     ErrorCodes.FailedToParse);
    }

    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}

function checkGlobalProfilerIsUpdated({testDB, oldFilter, newFilter}) {
    const log = assert.commandWorked(testDB.adminCommand({getLog: "global"})).log;
    assert(!!findMatchingLogLine(log, {
        msg: "Profiler settings changed globally",
        from: {filter: oldFilter},
        to: {filter: newFilter}
    }),
           "expected log line was not found");
}

function testGlobalFilterCommand(conn, isMongos) {
    const profileLevel = isMongos ? 0 : 1;
    const testDB = conn.getDB(jsTestName());
    const adminDB = conn.getDB("admin");
    const coll = testDB[collName];

    // Set the profiler filter globally.
    assert.commandWorked(testDB.runCommand({setProfilingFilterGlobally: 1, filter: filter}));
    checkGlobalProfilerIsUpdated({testDB: testDB, oldFilter: "none", newFilter: parsedFilter});

    runAggregationAndDowngradeFCV(coll, adminDB);

    // Change the profiler and check the response to show the filter was persisted from the FCV
    // downgrade.
    assert.commandWorked(testDB.runCommand({setProfilingFilterGlobally: 1, filter: {}}));
    checkGlobalProfilerIsUpdated({testDB: testDB, oldFilter: parsedFilter, newFilter: {}});

    if (isMongos) {
        // mongos does not sync FCV with the shards. The FCV on mongos will not be downgraded and
        // the filter should succeed to parse.
        assert.commandWorked(testDB.runCommand({profile: profileLevel, filter: filter}));
    } else {
        // We should not be able to make a profiler entry with the filter on the downgraded FCV.
        assert.commandFailedWithCode(testDB.runCommand({profile: profileLevel, filter: filter}),
                                     ErrorCodes.FailedToParse);
    }

    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}

(function testReplicaSet() {
    const rst = new ReplSetTest({
        nodes: [
            {setParameter: {internalQueryGlobalProfilingFilter: 1}},
            {setParameter: {internalQueryGlobalProfilingFilter: 1}}
        ]
    });
    rst.startSet();
    rst.initiate();

    const primaryConn = rst.getPrimary();
    testProfileCommand(primaryConn, false /* isMongos */);
    testGlobalFilterCommand(primaryConn, false /* isMongos */);
    rst.stopSet();
})();

(function testShardedCluster() {
    const st = new ShardingTest({
        shards: 1,
        rs: {nodes: 1},
        config: 1,
        mongosOptions: {setParameter: {internalQueryGlobalProfilingFilter: 1}}
    });
    testProfileCommand(st.s, true /* isMongos */);
    testGlobalFilterCommand(st.s, true /* isMongos */);

    st.stop();
})();
