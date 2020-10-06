/**
 * Tests that setting internalValidateFeaturesOnMaster on 4.4 and then
 * upgrading to latest does not cause server to crash and that the
 * original server parameter is still set.
 */
(function() {
"use strict";
load('./jstests/multiVersion/libs/multi_rs.js');  // Used for upgradeSet.

function runChecksBeforeUpgrade(db, internalValidateFeaturesBool) {
    // Verify that we can get the internalValidateFeaturesAsMaster parameter on 4.4.
    let res = db.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});
    assert.commandWorked(res);
    assert.eq(res.internalValidateFeaturesAsMaster, internalValidateFeaturesBool);

    // Verify that we cannot get the internalValidateFeaturesAsPrimary parameter on 4.4.
    res = db.adminCommand({getParameter: 1, internalValidateFeaturesAsPrimary: 1});
    assert.commandFailed(res);
}

function runChecksAfterUpgrade(db, port, internalValidateFeaturesBool) {
    // Verify that we can still get the internalValidateFeaturesAsMaster parameter on the latest
    // binary.
    let res = db.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});
    assert.commandWorked(res);
    assert.eq(res.internalValidateFeaturesAsMaster, internalValidateFeaturesBool);

    // However, trying to use the deprecated internalValidateFeaturesAsMaster parameter results
    // in a deprecation warning log.
    const joinShell = startParallelShell(
        "db.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});", port);
    joinShell();
    assert(rawMongoProgramOutput().match(
        "\"Use of deprecated server parameter name\",\"attr\":{\"deprecatedName\":\"internalValidateFeaturesAsMaster\""));

    // Verify that we can also get the internalValidateFeaturesAsPrimary parameter on the latest
    // binary.
    res = db.adminCommand({getParameter: 1, internalValidateFeaturesAsPrimary: 1});
    assert.commandWorked(res);
    assert.eq(res.internalValidateFeaturesAsPrimary, internalValidateFeaturesBool);
}

function replicaSetTest() {
    jsTestLog("Testing on replica set with version last-lts");
    const nodes = {
        0: {binVersion: "4.4"},
        1: {binVersion: "4.4"},
    };

    const rst = new ReplSetTest({nodes: nodes});
    rst.startSet();
    rst.initiate();
    let primary = rst.getPrimary();

    runChecksBeforeUpgrade(primary, true);

    jsTestLog("Upgrading set to latest");
    rst.upgradeSet({binVersion: "latest"});
    primary = rst.getPrimary();

    jsTestLog("Testing on replica set with version latest");
    runChecksAfterUpgrade(primary, primary.port, true);

    rst.stopSet();
}

function standaloneTest() {
    jsTestLog("Testing on standalone with version last-lts");
    let conn = MongoRunner.runMongod(
        {binVersion: "4.4", setParameter: "internalValidateFeaturesAsMaster=0"});
    assert.neq(null, conn, "mongod was unable to start up");

    let db = conn.getDB("admin");
    runChecksBeforeUpgrade(db, false);

    MongoRunner.stopMongod(conn);

    jsTest.log("Testing on standalone with version latest");
    conn = MongoRunner.runMongod(
        {binVersion: "latest", setParameter: "internalValidateFeaturesAsMaster=0"});
    assert.neq(null, conn, "mongod was unable to start up");

    db = conn.getDB("admin");
    runChecksAfterUpgrade(db, conn.port, false);

    MongoRunner.stopMongod(conn);
}

replicaSetTest();
standaloneTest();
})();
