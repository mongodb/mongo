'use strict';

function cleanupHelloFailInjection(connection) {
    jsTestLog(`Cleanup Hello fail injection in ${connection}`);
    let adminDB = connection.getDB('admin');
    assert.commandWorked(
        adminDB.runCommand({configureFailPoint: 'blockOrFailHelloCommand', mode: "off"}));
    const res = assert.commandWorked(
        adminDB.runCommand({getParameter: 1, "failpoint.blockOrFailHelloCommand": 1}));
    assert.eq(res["failpoint.blockOrFailHelloCommand"].mode, 0);
}

function doFailInjectionCleanup(db) {
    let connectionsToPrimaries = FixtureHelpers.getPrimaries(db);

    for (let connection of connectionsToPrimaries.concat(FixtureHelpers.getSecondaries(db))) {
        cleanupHelloFailInjection(connection);
    }
}

(function() {
    load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.
    load('jstests/libs/fixture_helpers.js');

    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
    var cmdLineOpts = db.adminCommand('getCmdLineOpts');
    const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
    jsTestLog(`Run Hello test suite cleanup in ${JSON.stringify(topology)},
               Invoked with ${JSON.stringify(cmdLineOpts)},
               topology type ${topology.type}`);

    if (topology.type === Topology.kShardedCluster) {
        doFailInjectionCleanup(db);
    }
    jsTestLog(`Hello fail hook completed`);
})();
