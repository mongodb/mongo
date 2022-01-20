/**
 * Tests that 'featureFlagExactTopNAccumulator' works correctly.
 *
 * TODO SERVER-61851 Delete this test once we branch for 6.0.
 *
 * @tags: [requires_replication, requires_sharding]
 */

(function() {
"use strict";

load("jstests/multiVersion/libs/verify_versions.js");
load("jstests/multiVersion/libs/multi_rs.js");       // For upgradeSecondaries and upgradeSet.
load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster.

function runTest(downgradeVersion) {
    const dbName = "db";
    const collName = "exactTopNUpgradeDowngrade";

    function getOperators(needTopBottom) {
        let obj = {needsInputAndN: ["$firstN", "$lastN", "$minN", "$maxN"]};
        if (needTopBottom) {
            Object.assign(
                obj, {needsOutputAndN: ["$topN", "$bottomN"], needsOutput: ["$top", "$bottom"]});
        }
        return obj;
    }

    function assertExpectedBehavior(expectedToPass, db) {
        function buildOperatorList(needAccumulators) {
            let operatorList = [];
            for (const [category, opNames] of Object.entries(getOperators(needAccumulators))) {
                for (const opName of opNames) {
                    let spec = {};
                    if (category === "needsInputAndN") {
                        Object.assign(spec, {input: "$a", n: 10});
                    } else if (category === "needsOutputAndN") {
                        Object.assign(spec, {output: "$a", n: 10, sortBy: {b: 1}});
                    } else if (category === "needsOutput") {
                        Object.assign(spec, {output: "$a", sortBy: {b: 1}});
                    }
                    operatorList.push({[opName]: spec});
                }
            }
            return operatorList;
        }

        function createView(pipeline, expectedErrorCodes) {
            const viewName = "testView";
            jsTestLog("Attempting to create view: " + tojson(pipeline));
            if (expectedToPass) {
                assert.commandWorked(db.createView(viewName, collName, pipeline));
                assert(db[viewName].drop());
            } else {
                assert.commandFailedWithCode(db.createView(viewName, collName, pipeline),
                                             expectedErrorCodes);
            }
        }

        function createValidator(aggExpr, expectedErrorCodes) {
            const collMod = {collMod: collName, validator: {$expr: aggExpr}};
            jsTestLog("Attempting to create validator: " + tojson(collMod));
            if (expectedToPass) {
                assert.commandWorked(db.runCommand(collMod));
            } else {
                assert.commandFailedWithCode(db.runCommand(collMod), expectedErrorCodes);
            }
        }

        // Accumulators.
        for (const accSpec of buildOperatorList(true)) {
            createView([{$group: {_id: "$groupKey", output: accSpec}}],
                       [15952, ErrorCodes.QueryFeatureNotAllowed]);
        }

        // Aggregation expressions.
        for (const aggExpr of buildOperatorList(false)) {
            createView([{$project: {output: aggExpr}}], [31325, ErrorCodes.QueryFeatureNotAllowed]);
            createValidator(
                aggExpr, [ErrorCodes.InvalidPipelineOperator, ErrorCodes.QueryFeatureNotAllowed]);
        }

        // Window functions.
        for (const wfExpr of buildOperatorList(true)) {
            const wfArg = Object.assign({window: {documents: [-1, 1]}}, wfExpr);
            createView(
                [{
                    $setWindowFields:
                        {partitionBy: "$partitionKey", sortBy: {sortField: 1}, output: {foo: wfArg}}
                }],
                [ErrorCodes.FailedToParse, ErrorCodes.QueryFeatureNotAllowed]);
        }
    }

    // Standalone.
    const dbPath = MongoRunner.dataPath + "/exact_top_n_upgrade_downgrade";
    let conn = MongoRunner.runMongod({dbpath: dbPath, binVersion: downgradeVersion});
    let testDB = conn.getDB(dbName);
    let coll = testDB[collName];
    assert.commandWorked(coll.insert({}));

    // This shouldn't pass in 'downgradeVersion'.
    assertExpectedBehavior(false, testDB, coll);

    // Upgrade the standalone to the latest binVersion; this still shouldn't pass.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(
        {binVersion: "latest", restart: conn, cleanData: false, dbpath: dbPath});
    testDB = conn.getDB(dbName);
    let adminDB = conn.getDB("admin");
    coll = testDB[collName];
    checkFCV(adminDB, downgradeVersion);
    assertExpectedBehavior(false, testDB, coll);

    // Set the FCV to 'latestFCV'; this should now pass.
    checkFCV(adminDB, downgradeVersion);
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);
    assertExpectedBehavior(true, testDB, coll);
    MongoRunner.stopMongod(conn);

    // Replica set.

    // Set up a replica-set in 'downgradeVersion'.
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: downgradeVersion}});
    rst.startSet();
    rst.initiate();

    let primary = rst.getPrimary();
    testDB = primary.getDB(dbName);
    coll = testDB[collName];
    assert.commandWorked(coll.insert({}));

    // Shouldn't pass in 'downgradeVersion'.
    assertExpectedBehavior(false, testDB);

    // Upgrade the replica set.
    rst.upgradeSet({binVersion: "latest"});

    // Verify that all nodes are in the latest version.
    for (const node of rst.nodes) {
        assert.binVersion(node, "latest");
    }

    rst.awaitNodesAgreeOnPrimary();
    primary = rst.getPrimary();
    testDB = primary.getDB(dbName);

    // Despite the upgrade, the test shouldn't pass because the FCV has not been explicitly set.
    assertExpectedBehavior(false, testDB);

    // Set the FCV; the test should now pass.
    primary = rst.getPrimary();
    adminDB = primary.getDB("admin");
    checkFCV(adminDB, downgradeVersion);
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);
    testDB = primary.getDB(dbName);
    assertExpectedBehavior(true, testDB);
    rst.stopSet();

    // Sharded cluster.
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 2, binVersion: downgradeVersion},
        other: {
            mongosOptions: {binVersion: downgradeVersion},
            configOptions: {binVersion: downgradeVersion}
        }
    });

    testDB = st.s.getDB(dbName);
    coll = testDB[collName];
    assert.commandWorked(coll.insert({}));

    // The test shouldn't pass in 'downgradeVersion'.
    adminDB = st.s.getDB("admin");
    checkFCV(adminDB, downgradeVersion);
    assertExpectedBehavior(false, testDB);

    // Upgrade the cluster.
    st.upgradeCluster("latest", {waitUntilStable: true});
    testDB = st.s.getDB(dbName);

    // Despite the upgrade, the test shouldn't pass because the FCV has not been explicitly set.
    adminDB = st.s.getDB("admin");
    checkFCV(adminDB, downgradeVersion);
    assertExpectedBehavior(false, testDB);

    // Set the FCV; the test should now pass.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);
    assertExpectedBehavior(true, testDB);
    st.stop();
}

runFeatureFlagMultiversionTest("featureFlagExactTopNAccumulator", runTest);
})();
