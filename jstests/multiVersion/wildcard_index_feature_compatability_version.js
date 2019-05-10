/**
 * Test that wildcard indexes obey the following FCV behaviour:
 * - Cannot be built on 4.0, or on 4.2 under FCV 4.0.
 * - A $** index built on FCV 4.2 can still be used under FCV 4.0.
 * - An FCV 4.0 node can restart with a $** index present in its catalog.
 * - A $** index built on FCV 4.2 can be sync'd by a 4.0 FCV Secondary.
 * - A $** index can be dropped while in FCV 4.0.
 * - A downgraded 4.0 node with a $** index fails to start due to Fatal Assertion 28782.
 */
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");           // For isCollscan.
    load("jstests/multiVersion/libs/multi_rs.js");  // For upgradeSet.
    load("jstests/replsets/rslib.js");              // For startSetIfSupportsReadMajority.

    TestData.skipCheckDBHashes = true;  // Skip db hashes when restarting the replset.

    const nodeOptions40 = {binVersion: "last-stable"};
    const nodeOptions42 = {binVersion: "latest"};

    // Set up a new replSet consisting of 3 nodes, initially running on 4.0 binaries.
    const rst = new ReplSetTest({nodes: 3, nodeOptions: nodeOptions40});

    if (!startSetIfSupportsReadMajority(rst)) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        rst.stopSet();
        return;
    }

    rst.initiate();

    let testDB = rst.getPrimary().getDB(jsTestName());
    let coll = testDB.wildcard_index_fcv;
    coll.drop();

    // Verifies that the instance is running with the specified binary version and FCV.
    function assertVersionAndFCV(db, versions, fcv) {
        const majorMinorVersion = db.version().substring(0, 3);
        versions = (Array.isArray(versions) ? versions : [versions]);
        assert(versions.includes(majorMinorVersion));
        assert.eq(
            assert.commandWorked(db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}))
                .featureCompatibilityVersion.version,
            fcv);
    }

    // Restarts the given replset nodes, or the entire replset if no nodes are specified.
    function restartReplSetNodes(replSet, nodes, options) {
        const defaultOpts = {remember: true, appendOptions: true, startClean: false};
        options = Object.assign(defaultOpts, (options || {}));
        nodes = (nodes || replSet.nodes);
        assert(Array.isArray(nodes));
        for (let node of nodes) {
            // Merge the new options into the existing options for the given nodes.
            Object.assign(replSet.nodeOptions[`n${replSet.getNodeId(node)}`], options);
        }
        replSet.restart(nodes, options);
    }

    // Verify that the replset is on binary version 4.0 and FCV 4.0.
    assertVersionAndFCV(testDB, "4.0", "4.0");

    jsTestLog("Cannot create a $** index on a replset running binary 4.0.");
    assert.commandFailedWithCode(coll.createIndex({"$**": 1}), ErrorCodes.CannotCreateIndex);

    // Upgrade the set to the new binary version, but keep the feature compatibility version at 4.0.
    rst.upgradeSet(nodeOptions42);
    testDB = rst.getPrimary().getDB(jsTestName());
    coll = testDB.wildcard_index_fcv;
    assertVersionAndFCV(testDB, ["4.1", "4.2"], "4.0");

    jsTestLog("Cannot create a $** index on binary 4.2 with FCV 4.0.");
    assert.commandFailedWithCode(coll.createIndex({"$**": 1}), ErrorCodes.CannotCreateIndex);

    jsTestLog("Can create a $** index on binary 4.2 with FCV 4.2.");
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.2"}));
    assert.commandWorked(coll.createIndex({"$**": 1}));
    assert.commandWorked(coll.insert([{a: 1, b: 1}, {a: 2, b: 2}]));
    rst.awaitReplication();

    // Confirm that the index can be used to answer queries.
    let explainOutput = assert.commandWorked(coll.find({a: {$gt: 1}}).explain()).queryPlanner;
    assert(!isCollscan(testDB, explainOutput.winningPlan), () => tojson(explainOutput));

    jsTestLog("Can use an existing $** after downgrading FCV to 4.0.");
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    explainOutput = assert.commandWorked(coll.find({b: {$gt: 1}}).explain()).queryPlanner;
    assert(!isCollscan(testDB, explainOutput.winningPlan), () => tojson(explainOutput));

    jsTestLog("Cannot create a new $** after downgrading FCV to 4.0.");
    let coll_other = testDB.wildcard_index_fcv_other;
    assert.commandFailedWithCode(coll_other.createIndex({"$**": 1}), ErrorCodes.CannotCreateIndex);

    jsTestLog("Can restart the replset in FCV 4.0 with a $** index present.");
    restartReplSetNodes(rst);
    testDB = rst.getPrimary().getDB(jsTestName());
    coll = testDB.wildcard_index_fcv;
    assertVersionAndFCV(testDB, ["4.1", "4.2"], "4.0");

    // Verify that we can still successfully run queries on the $** index.
    explainOutput = assert.commandWorked(coll.find({a: {$gt: 1}}).explain()).queryPlanner;
    assert(!isCollscan(testDB, explainOutput.winningPlan), () => tojson(explainOutput));

    jsTestLog("Can restart the Secondaries in FCV 4.0 and resync the $** index from the Primary.");
    restartReplSetNodes(rst, rst.getSecondaries(), {startClean: true});
    rst.awaitSecondaryNodes();
    rst.awaitReplication();
    // Verify that the Secondaries have both recreated the $** index.
    let secondaries = rst.getSecondaries();
    assert.eq(secondaries.length, 2);
    for (let sec of secondaries) {
        assert.eq(sec.getCollection(coll.getFullName())
                      .aggregate([{$indexStats: {}}, {$match: {"key.$**": 1}}])
                      .toArray()
                      .length,
                  1);
    }

    jsTestLog("Can drop an existing $** index in FCV 4.0.");
    assert.commandWorked(coll.dropIndex({"$**": 1}));

    jsTestLog("Cannot recreate the dropped $** index in FCV 4.0.");
    assert.commandFailedWithCode(coll.createIndex({"$**": 1}), ErrorCodes.CannotCreateIndex);

    // Set the FCV to 4.2 and re-create the $** index. We need to test that a 4.0 binary fails to
    // start when a wildcard index that was built on 4.2 is still present in the catalog.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.2"}));
    assert.commandWorked(coll.createIndex({"$**": 1}));

    jsTestLog("Cannot start 4.0 binary with $** index present.");
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    assertVersionAndFCV(testDB, ["4.1", "4.2"], "4.0");
    secondaries = rst.getSecondaries();
    assert.eq(secondaries.length, 2);
    rst.awaitReplication();
    try {
        restartReplSetNodes(rst, [secondaries[0]], nodeOptions40);
        assert(false, "Expected 'restartReplSetNodes' to throw");
    } catch (err) {
        assert.eq(err.message, `Failed to start node ${rst.getNodeId(secondaries[0])}`);
        // In most cases we expect the node to fail with 28782 because it sees the wildcard index in
        // its catalog on startup and doesn't recognize the format. However in some cases the node
        // will start up having not completely persisted the index build before shutting down. In
        // these cases the node will attempt to re-build the index on startup and encounter a
        // different error (40590) upon trying to rebuild the wildcard index.
        assert(rawMongoProgramOutput().match("Fatal Assertion 28782") ||
               rawMongoProgramOutput().match("Fatal Assertion 40590"));
    }

    jsTestLog("Restart the failed node on binary 4.2 and gracefully shut down the replset.");
    Object.assign(rst.nodeOptions[`n${rst.getNodeId(secondaries[0])}`], nodeOptions42);
    rst.start(secondaries[0], nodeOptions42);

    rst.awaitReplication();
    rst.stopSet();
}());
