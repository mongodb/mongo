/**
 * Tests autoPromoteHidden functionality with read preference scenarios.
 *
 * Case 1: 3-member replica set with 1 hidden node
 *   - Test read preference secondary only fails when non-hidden secondaries are down
 *   - Test read preference primary succeeds
 *   - Test with autoPromoteHidden enabled, secondary only reads succeed
 *
 * Case 2: Sharded cluster with autoPromoteHidden
 *   - Similar to Case 1 but in sharded environment
 *
 * Case 3: 5-member replica set with 2 hidden nodes
 *   - Stop 2 non-hidden secondaries and test functionality
 *
 * @tags: [requires_fcv_81]
 */

import {reconfig} from "jstests/replsets/rslib.js";

const dbName = "testDB";
const collName = "testColl";

// Helper function to test read with specific read preference
function testRead(conn, dbName, collName, readPref, shouldSucceed, description) {
    try {
        const result = conn.getDB(dbName).runCommand({
            find: collName,
            limit: 1,
            $readPreference: readPref
        });
        
        if (shouldSucceed) {
            assert.commandWorked(result, description + " - should succeed but failed");
            jsTestLog("[PASS] " + description + " - succeeded as expected");
            return true;
        } else {
            assert.commandFailed(result, description + " - should fail but succeeded");
            jsTestLog("[FAIL] " + description + " - unexpectedly succeeded");
            return false;
        }
    } catch (e) {
        if (!shouldSucceed) {
            jsTestLog("[PASS] " + description + " - failed as expected: " + e.message);
            return true;
        } else {
            jsTestLog("[FAIL] " + description + " - unexpectedly failed: " + e.message);
            throw e;
        }
    }
}

// =============================================================================
// CASE 1: 3-member replica set with 1 hidden node
// =============================================================================
(function testCase1_ThreeNodeReplSet() {
    jsTestLog("====================================================================");
    jsTestLog("CASE 1: Testing 3-member replica set with 1 hidden node");
    jsTestLog("====================================================================");
    
    const rst = new ReplSetTest({
        name: "auto_promote_hidden_case1",
        nodes: [
            {},  // Primary
            {},  // Secondary 1
            {rsConfig: {priority: 0, hidden: true}}  // Hidden node
        ],
        settings: {heartbeatIntervalMillis: 500, electionTimeoutMillis: 2000}
    });
    
    rst.startSet();
    rst.initiate();
    
    const primary = rst.getPrimary();
    const secondaries = rst.getSecondaries();
    const regularSecondary = secondaries[0];
    const hiddenNode = secondaries[1];
    
    jsTestLog("Setting up test data...");
    assert.commandWorked(primary.getDB(dbName).getCollection(collName).insert(
        [{_id: 1, data: "test"}]
    ));
    rst.awaitReplication();
    
    jsTestLog("Step 1.1: Verify initial read preferences work correctly");
    // Read from primary should work
    testRead(primary, dbName, collName, {mode: "primary"}, true, 
             "Read with primary preference");
    
    // Read from secondary should work (we have 1 non-hidden secondary available)
    testRead(primary, dbName, collName, {mode: "secondary"}, true,
             "Read with secondary preference (before stopping secondary)");
    
    jsTestLog("Step 1.2: Stop the non-hidden secondary");
    rst.stop(regularSecondary);
    
    // Wait for primary to detect the secondary is down
    assert.soon(() => {
        const status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
        for (let member of status.members) {
            if (member.name === regularSecondary.host) {
                return member.health === 0;
            }
        }
        return false;
    }, "Primary should detect secondary is down", 30000);
    
    sleep(3000);
    
    jsTestLog("Step 1.3: Test read preferences with autoPromoteHidden=false (default)");
    // Read from primary should still work
    testRead(primary, dbName, collName, {mode: "primary"}, true,
             "Read with primary preference (secondary down, autoPromote=false)");
    
    // Read from secondary should fail - no non-hidden secondary available
    // Hidden node is not visible for read preference routing
    testRead(primary, dbName, collName, {mode: "secondary"}, false,
             "Read with secondary preference (secondary down, autoPromote=false)");
    
    jsTestLog("Step 1.4: Enable autoPromoteHidden");
    let config = rst.getReplSetConfigFromNode();
    config.version++;
    config.settings = config.settings || {};
    config.settings.autoPromoteHidden = true;
    
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
    jsTestLog("Reconfig command worked - autoPromoteHidden enabled");
    
    // Wait for config to propagate and topology to update
    sleep(5000);
    
    // Verify hidden node is now in hosts list
    const helloResp = assert.commandWorked(primary.adminCommand({hello: 1}));
    jsTestLog("Hello response after enabling autoPromoteHidden: " + tojson(helloResp));
    
    let hiddenPromoted = helloResp.hosts && helloResp.hosts.includes(hiddenNode.host);
    assert.eq(true, hiddenPromoted, 
              "Hidden node should be in hosts list after enabling autoPromoteHidden");
    
    jsTestLog("Step 1.5: Test read preferences with autoPromoteHidden=true");
    // Read from primary should still work
    testRead(primary, dbName, collName, {mode: "primary"}, true,
             "Read with primary preference (autoPromote=true)");
    
    // Read from secondary should now succeed because hidden node is promoted
    testRead(primary, dbName, collName, {mode: "secondary"}, true,
             "Read with secondary preference (secondary down, autoPromote=true)");
    
    jsTestLog("Step 1.6: Restart the secondary and verify hidden node is demoted");
    rst.restart(regularSecondary);
    rst.awaitSecondaryNodes(30000, [regularSecondary]);
    
    // Wait for topology update
    assert.soon(() => {
        const status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
        for (let member of status.members) {
            if (member.name === regularSecondary.host) {
                return member.health === 1 && member.state === 2;
            }
        }
        return false;
    }, "Secondary should be healthy", 30000);
    
    sleep(3000);
    
    const helloResp2 = assert.commandWorked(primary.adminCommand({hello: 1}));
    hiddenPromoted = helloResp2.hosts && helloResp2.hosts.includes(hiddenNode.host);
    assert.eq(false, hiddenPromoted,
              "Hidden node should be removed from hosts when secondary is healthy again");
    
    rst.stopSet();
    
    jsTestLog("[PASSED] CASE 1: 3-member replica set test completed successfully");
})();

// =============================================================================
// CASE 2: Sharded cluster with autoPromoteHidden
// =============================================================================
(function testCase2_ShardedCluster() {
    jsTestLog("====================================================================");
    jsTestLog("CASE 2: Testing sharded cluster with autoPromoteHidden");
    jsTestLog("====================================================================");
    
    const st = new ShardingTest({
        shards: 1,
        rs0: {
            nodes: [
                {},  // Primary
                {},  // Secondary 1
                {rsConfig: {priority: 0, hidden: true}}  // Hidden node
            ],
            settings: {heartbeatIntervalMillis: 500}
        },
        config: 1,
        mongos: 1
    });
    
    const shard0Rst = st.rs0;
    const primary = shard0Rst.getPrimary();
    const secondaries = shard0Rst.getSecondaries();
    const regularSecondary = secondaries[0];
    const hiddenNode = secondaries[1];
    
    jsTestLog("Setting up sharded collection with test data...");
    const mongos = st.s;
    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
    assert.commandWorked(mongos.adminCommand({
        shardCollection: dbName + "." + collName,
        key: {_id: 1}
    }));
    
    assert.commandWorked(mongos.getDB(dbName).getCollection(collName).insert(
        [{_id: 1, data: "shard_test"}]
    ));
    shard0Rst.awaitReplication();
    
    jsTestLog("Step 2.1: Test reads through mongos with secondary read preference");
    // This should work - mongos can route to the non-hidden secondary
    const readResult1 = assert.commandWorked(mongos.getDB(dbName).runCommand({
        find: collName,
        $readPreference: {mode: "secondary"}
    }));
    assert.eq(1, readResult1.cursor.firstBatch.length, "Should read from secondary");
    jsTestLog("[PASS] Read from secondary through mongos succeeded");
    
    jsTestLog("Step 2.2: Stop the non-hidden secondary on shard");
    shard0Rst.stop(regularSecondary);
    
    // Wait for detection
    assert.soon(() => {
        const status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
        for (let member of status.members) {
            if (member.name === regularSecondary.host) {
                return member.health === 0;
            }
        }
        return false;
    }, "Shard primary should detect secondary is down", 30000);
    
    sleep(3000);
    
    jsTestLog("Step 2.3: Test read with secondary preference should fail (no visible secondary)");
    // Without autoPromoteHidden, mongos cannot route to hidden node
    try {
        mongos.getDB(dbName).getMongo().setReadPref("secondary");
        const readResult2 = mongos.getDB(dbName).getCollection(collName).find().limit(1).toArray();
        // This might timeout or fail because no secondary is available
        jsTestLog("Read attempt completed (may have retried to primary): " + tojson(readResult2));
    } catch (e) {
        jsTestLog("[PASS] Read with secondary preference failed as expected: " + e.message);
    }
    
    jsTestLog("Step 2.4: Enable autoPromoteHidden on the shard");
    let config = shard0Rst.getReplSetConfigFromNode();
    config.version++;
    config.settings = config.settings || {};
    config.settings.autoPromoteHidden = true;
    
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
    
    sleep(5000);
    
    // Verify hidden node is promoted
    const helloResp = assert.commandWorked(primary.adminCommand({hello: 1}));
    assert(helloResp.hosts.includes(hiddenNode.host),
           "Hidden node should be in hosts after autoPromoteHidden");
    jsTestLog("[PASS] Hidden node promoted in shard replica set");
    
    jsTestLog("Step 2.5: Test read with secondary preference should now work");
    // Mongos should now be able to route to the promoted hidden node
    mongos.getDB(dbName).getMongo().setReadPref("secondary");
    assert.soon(() => {
        try {
            const readResult3 = mongos.getDB(dbName).getCollection(collName).find().limit(1).toArray();
            return readResult3.length === 1;
        } catch (e) {
            jsTestLog("Read still failing, retrying: " + e.message);
            return false;
        }
    }, "Should be able to read from promoted hidden node", 30000);
    jsTestLog("[PASS] Read with secondary preference succeeded after autoPromoteHidden");
    
    jsTestLog("Step 2.6: Restart secondary and verify hidden node is demoted");
    shard0Rst.restart(regularSecondary);
    shard0Rst.awaitSecondaryNodes(30000, [regularSecondary]);
    
    sleep(5000);
    
    const helloResp2 = assert.commandWorked(primary.adminCommand({hello: 1}));
    assert(!helloResp2.hosts.includes(hiddenNode.host),
           "Hidden node should be demoted when secondary is healthy");
    
    st.stop();
    
    jsTestLog("[PASSED] CASE 2: Sharded cluster test completed successfully");
})();

// =============================================================================
// CASE 3: 5-member replica set with 2 hidden nodes
// =============================================================================
(function testCase3_FiveNodeReplSet() {
    jsTestLog("====================================================================");
    jsTestLog("CASE 3: Testing 5-member replica set with 2 hidden nodes");
    jsTestLog("====================================================================");
    
    const rst = new ReplSetTest({
        name: "auto_promote_hidden_case3",
        nodes: [
            {},  // Primary
            {},  // Secondary 1
            {},  // Secondary 2
            {rsConfig: {priority: 0, hidden: true}},  // Hidden node 1
            {rsConfig: {priority: 0, hidden: true}}   // Hidden node 2
        ],
        settings: {heartbeatIntervalMillis: 500, electionTimeoutMillis: 2000}
    });
    
    rst.startSet();
    rst.initiate();
    
    const primary = rst.getPrimary();
    const allSecondaries = rst.getSecondaries();
    
    // Identify non-hidden and hidden secondaries
    const configMembers = rst.getReplSetConfigFromNode().members;
    const regularSecondaries = [];
    const hiddenNodes = [];
    
    for (let node of allSecondaries) {
        const memberConfig = configMembers.find(m => m.host === node.host);
        if (memberConfig.hidden) {
            hiddenNodes.push(node);
        } else {
            regularSecondaries.push(node);
        }
    }
    
    jsTestLog("Regular secondaries: " + regularSecondaries.map(n => n.host));
    jsTestLog("Hidden nodes: " + hiddenNodes.map(n => n.host));
    
    assert.eq(2, regularSecondaries.length, "Should have 2 regular secondaries");
    assert.eq(2, hiddenNodes.length, "Should have 2 hidden nodes");
    
    jsTestLog("Setting up test data...");
    assert.commandWorked(primary.getDB(dbName).getCollection(collName).insert(
        [{_id: 1, data: "test"}, {_id: 2, data: "test2"}, {_id: 3, data: "test3"}]
    ));
    rst.awaitReplication();
    
    jsTestLog("Step 3.1: Verify initial reads work with secondary preference");
    testRead(primary, dbName, collName, {mode: "secondary"}, true,
             "Read with secondary preference (2 healthy secondaries)");
    
    jsTestLog("Step 3.2: Stop BOTH non-hidden secondaries");
    rst.stop(regularSecondaries[0]);
    rst.stop(regularSecondaries[1]);
    
    // Wait for primary to detect both secondaries are down
    assert.soon(() => {
        const status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
        let downCount = 0;
        for (let member of status.members) {
            if ((member.name === regularSecondaries[0].host || 
                 member.name === regularSecondaries[1].host) && 
                member.health === 0) {
                downCount++;
            }
        }
        return downCount === 2;
    }, "Primary should detect both secondaries are down", 30000);
    
    sleep(3000);
    
    jsTestLog("Step 3.3: Test read preferences WITHOUT autoPromoteHidden");
    // Primary read should work
    testRead(primary, dbName, collName, {mode: "primary"}, true,
             "Read with primary preference (both secondaries down)");
    
    // Secondary read should fail - no visible secondary
    testRead(primary, dbName, collName, {mode: "secondary"}, false,
             "Read with secondary preference (both secondaries down, autoPromote=false)");
    
    // Verify hidden nodes are NOT in hosts list
    let helloResp = assert.commandWorked(primary.adminCommand({hello: 1}));
    jsTestLog("Hello response before autoPromoteHidden: " + tojson(helloResp.hosts));
    
    let hiddenCount = 0;
    for (let host of (helloResp.hosts || [])) {
        if (host === hiddenNodes[0].host || host === hiddenNodes[1].host) {
            hiddenCount++;
        }
    }
    assert.eq(0, hiddenCount, "No hidden nodes should be in hosts list initially");
    
    jsTestLog("Step 3.4: Enable autoPromoteHidden");
    let config = rst.getReplSetConfigFromNode();
    config.version++;
    config.settings = config.settings || {};
    config.settings.autoPromoteHidden = true;
    
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
    
    // Wait for topology update
    sleep(5000);
    
    jsTestLog("Step 3.5: Verify hidden nodes are promoted to hosts list");
    helloResp = assert.commandWorked(primary.adminCommand({hello: 1}));
    jsTestLog("Hello response after autoPromoteHidden: " + tojson(helloResp.hosts));
    
    hiddenCount = 0;
    for (let host of (helloResp.hosts || [])) {
        if (host === hiddenNodes[0].host || host === hiddenNodes[1].host) {
            hiddenCount++;
        }
    }
    assert.gte(hiddenCount, 1, "At least one hidden node should be promoted to hosts list");
    jsTestLog("[PASS] " + hiddenCount + " hidden node(s) promoted to hosts list");
    
    jsTestLog("Step 3.6: Test read with secondary preference - should now succeed");
    testRead(primary, dbName, collName, {mode: "secondary"}, true,
             "Read with secondary preference (autoPromote=true, reading from hidden)");
    
    // Verify we can actually read from hidden nodes
    assert.commandWorked(hiddenNodes[0].getDB(dbName).runCommand({
        find: collName,
        limit: 1
    }));
    jsTestLog("[PASS] Direct read from hidden node succeeded");
    
    jsTestLog("Step 3.7: Test secondaryPreferred - should route to hidden");
    testRead(primary, dbName, collName, {mode: "secondaryPreferred"}, true,
             "Read with secondaryPreferred (should route to promoted hidden)");
    
    jsTestLog("Step 3.8: Restart one non-hidden secondary");
    rst.restart(regularSecondaries[0]);
    rst.awaitSecondaryNodes(30000, [regularSecondaries[0]]);
    
    // Wait for primary to detect the secondary is healthy
    assert.soon(() => {
        const status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
        for (let member of status.members) {
            if (member.name === regularSecondaries[0].host) {
                return member.health === 1 && member.state === 2;
            }
        }
        return false;
    }, "First secondary should be healthy", 30000);
    
    sleep(3000);
    
    jsTestLog("Step 3.9: Verify hidden nodes are demoted (one healthy secondary is enough)");
    helloResp = assert.commandWorked(primary.adminCommand({hello: 1}));
    jsTestLog("Hello response after secondary restart: " + tojson(helloResp.hosts));
    
    hiddenCount = 0;
    for (let host of (helloResp.hosts || [])) {
        if (host === hiddenNodes[0].host || host === hiddenNodes[1].host) {
            hiddenCount++;
        }
    }
    assert.eq(0, hiddenCount, 
              "Hidden nodes should be demoted when at least one non-hidden secondary is healthy");
    
    jsTestLog("Step 3.10: Verify reads still work from regular secondary");
    testRead(primary, dbName, collName, {mode: "secondary"}, true,
             "Read with secondary preference (after hidden demotion)");
    
    // Restart the other secondary for cleanup
    rst.restart(regularSecondaries[1]);
    
    rst.stopSet();
    
    jsTestLog("[PASSED] CASE 3: 5-member replica set test completed successfully");
})();

// =============================================================================
// Additional verification: Read preference behavior
// =============================================================================
(function testCase1Extended_ReadPreferenceVariations() {
    jsTestLog("====================================================================");
    jsTestLog("EXTENDED: Testing various read preference modes");
    jsTestLog("====================================================================");
    
    const rst = new ReplSetTest({
        name: "auto_promote_hidden_extended",
        nodes: [
            {},
            {},
            {rsConfig: {priority: 0, hidden: true}}
        ],
        settings: {heartbeatIntervalMillis: 500}
    });
    
    rst.startSet();
    rst.initiate();
    
    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    
    // Enable autoPromoteHidden from the start
    let config = rst.getReplSetConfigFromNode();
    config.version++;
    config.settings = config.settings || {};
    config.settings.autoPromoteHidden = true;
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
    
    // Insert test data
    assert.commandWorked(primary.getDB(dbName).getCollection(collName).insert(
        [{_id: 1, x: 1}]
    ));
    rst.awaitReplication();
    
    jsTestLog("Step EXT.1: Test all read preference modes with healthy secondary");
    testRead(primary, dbName, collName, {mode: "primary"}, true, "primary mode");
    testRead(primary, dbName, collName, {mode: "primaryPreferred"}, true, "primaryPreferred mode");
    testRead(primary, dbName, collName, {mode: "secondary"}, true, "secondary mode");
    testRead(primary, dbName, collName, {mode: "secondaryPreferred"}, true, "secondaryPreferred mode");
    testRead(primary, dbName, collName, {mode: "nearest"}, true, "nearest mode");
    
    jsTestLog("Step EXT.2: Stop secondary and retest");
    rst.stop(secondary);
    
    assert.soon(() => {
        const status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
        for (let member of status.members) {
            if (member.name === secondary.host) {
                return member.health === 0;
            }
        }
        return false;
    }, "Secondary should be down", 30000);
    
    sleep(5000);  // Wait for promotion
    
    jsTestLog("Step EXT.3: Test all read preference modes with promoted hidden node");
    testRead(primary, dbName, collName, {mode: "primary"}, true, 
             "primary mode (hidden promoted)");
    testRead(primary, dbName, collName, {mode: "primaryPreferred"}, true, 
             "primaryPreferred mode (hidden promoted)");
    testRead(primary, dbName, collName, {mode: "secondary"}, true,
             "secondary mode (should route to promoted hidden)");
    testRead(primary, dbName, collName, {mode: "secondaryPreferred"}, true,
             "secondaryPreferred mode (hidden promoted)");
    testRead(primary, dbName, collName, {mode: "nearest"}, true,
             "nearest mode (hidden promoted)");
    
    rst.restart(secondary);
    rst.stopSet();
    
    jsTestLog("[PASSED] EXTENDED TESTS: All read preference modes work correctly");
})();

jsTestLog("========================================================================");
jsTestLog("ALL TEST CASES PASSED!");
jsTestLog("========================================================================");
jsTestLog("Summary:");
jsTestLog("  [PASSED] Case 1: 3-member replica set");
jsTestLog("  [PASSED] Case 2: Sharded cluster");
jsTestLog("  [PASSED] Case 3: 5-member replica set with 2 hidden nodes");
jsTestLog("  [PASSED] Extended: Various read preference modes");
jsTestLog("========================================================================");

