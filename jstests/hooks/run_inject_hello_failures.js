'use strict';

// Interval between test loops.
const kTestLoopPeriodMs = 20 * 1000;

// Sleep injected to the Hello response at the server side.
const kInjectedHelloDelayMs = 6000 * 1000;

// How many times the fail inject - stepdown cycle is repeated.
const kTestLoops = 1;

// Refresh timeout will be reduced to this interval.
const kRefreshTimeoutSec = 1;

// Connection could be to 'mongos' or 'mongod'.
function getAdminDB(connection) {
    let adminDB;
    if (typeof connection.getDB === 'function') {
        adminDB = connection.getDB('admin');
    } else {
        assert(typeof connection.getSiblingDB === 'function',
               `Cannot get Admin DB from ${tojson(connection)}`);
        adminDB = connection.getSiblingDB('admin');
    }
    return adminDB;
}

function stepDown(connection) {
    jsTestLog(`Force stepDown to ${connection}`);
    const adminDB = getAdminDB(connection);
    let res;
    let error;
    try {
        res = adminDB.runCommand({replSetStepDown: 10, force: true, secondaryCatchUpPeriodSecs: 8});
        error = res;
    } catch (e) {
        error = e;
        jsTestLog(`Step down error is usually normal: ${error}`);
    }
    if (error && (error.code == undefined || error.code == ErrorCodes.HostUnreachable)) {
        jsTestLog(`Transient error ${error}`);
        return;
    }
    assert.commandWorked(res);
    jsTestLog(`Forced step down to ${connection}, result ${res}`);
}

function stepUp(connection) {
    const adminDB = getAdminDB(connection);
    assert.soonNoExcept(() => {
        const res = adminDB.runCommand({replSetStepUp: 1});
        if (!res.ok) {
            jsTestLog(`Failed to step up with ${res}`);
        }
        return res.ok;
    }, "Failed to step up");
    jsTestLog(`Forced step up to ${connection}`);
}

// The default interval of 30 sec between RSM refresh cycles is too long for
// this test.
function injectReduceRefreshPeriod(connection) {
    jsTestLog(`Reduce refresh interval for ${connection}`);
    const adminDB = getAdminDB(connection);
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "modifyReplicaSetMonitorDefaultRefreshPeriod",
        mode: "alwaysOn",
        data: {period: kRefreshTimeoutSec},
    }));
    const res = adminDB.runCommand(
        {getParameter: 1, "failpoint.modifyReplicaSetMonitorDefaultRefreshPeriod": 1});
    assert.commandWorked(res);
    assert.eq(res["failpoint.modifyReplicaSetMonitorDefaultRefreshPeriod"].mode, 1);
}

function injectHelloFail(connection) {
    jsTestLog(`Inject Hello fail to connection ${connection}`);
    const adminDB = getAdminDB(connection);
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: 'waitInHello',
        mode: "alwaysOn",
        data: {
            internalClient: 1,  // No effect if client is mongo shell.
            delay: kInjectedHelloDelayMs
        }
    }));
    const res = adminDB.runCommand({getParameter: 1, "failpoint.waitInHello": 1});
    assert.commandWorked(res);
    assert.eq(res["failpoint.waitInHello"].mode, 1);
}

function freeze(connection) {
    const adminDB = getAdminDB(connection);
    assert.commandWorked(adminDB.runCommand({replSetFreeze: 20}));
}

function getConfigServer(connection) {
    const adminDB = getAdminDB(connection);
    const res = assert.commandWorked(adminDB.runCommand({serverStatus: 1}))
                    .sharding.configsvrConnectionString;
    var rx = /.*\/(.*)/g;
    var arr = rx.exec(res);
    jsTestLog(`Config server: ${arr[1]} extracted from ${tojson(res)}`);
    return new Mongo(arr[1]);
}

function doFailInjectionLoop(db) {
    for (let counter = 0; counter < kTestLoops; ++counter) {
        let connectionsToPrimaries;
        let connectionsToSecondaries = [];
        let configServer;
        try {
            connectionsToPrimaries = FixtureHelpers.getPrimaries(db);
            let allReplicaSets = FixtureHelpers.getAllReplicas(db);
            for (let replicaSet of allReplicaSets) {
                connectionsToSecondaries.push(replicaSet.getSecondaries());
            }
            configServer = getConfigServer(db);
        } catch (e) {
            jsTestLog(`Cannot fetch primaries or secondaries: ${e}`);
            sleep(kTestLoopPeriodMs);
            continue;
        }
        // This will reduce refresh timeout on mongos and config server as well.
        injectReduceRefreshPeriod(db);
        injectReduceRefreshPeriod(configServer);
        for (let connection of connectionsToPrimaries.concat(FixtureHelpers.getSecondaries(db))) {
            injectReduceRefreshPeriod(connection);
        }
        // The tests usually have 10-20 sec timeout on operations. The default refresh period is 30
        // sec.
        // After we reduced the refresh timeout we need to wait for the previously scheduled
        // timeouts to
        // approach before injecting the Hello delay failure.
        sleep(25 * 1000);
        for (let connection of connectionsToPrimaries) {
            injectHelloFail(connection);
        }
        for (let connection of connectionsToPrimaries) {
            stepDown(connection);
            freeze(connection);
        }
        for (let arrayOfSecondaries of connectionsToSecondaries) {
            for (let connection of arrayOfSecondaries) {
                stepUp(connection);
                break;  // For each replica set pick one secondary.
            }
        }
        sleep(kTestLoopPeriodMs);
    }
}

(function() {
load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.
load('jstests/libs/fixture_helpers.js');

assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
var cmdLineOpts = db.adminCommand('getCmdLineOpts');
const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
jsTestLog(`Run Hello fail injection in ${JSON.stringify(topology)},
               Invoked with ${JSON.stringify(cmdLineOpts)},
               topology type ${topology.type}`);

if (topology.type === Topology.kShardedCluster) {
    doFailInjectionLoop(db);
}
jsTestLog(`Hello fail hook completed`);
})();
