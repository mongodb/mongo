import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {getFailPointName} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

function cleanupHelloFailInjection(connection) {
    jsTestLog(`Cleanup Hello fail injection in ${connection}`);
    let adminDB = connection.getDB('admin');
    const fpName = getFailPointName("shardWaitInHello", connection.getMaxWireVersion());
    assert.commandWorked(adminDB.runCommand({configureFailPoint: fpName, mode: "off"}));
    const prefixedName = `failpoint.${fpName}`;
    const res = assert.commandWorked(adminDB.runCommand({getParameter: 1, [prefixedName]: 1}));
    assert.eq(res[`failpoint.${fpName}`].mode, 0);
}

function doFailInjectionCleanup(db) {
    let connectionsToPrimaries = FixtureHelpers.getPrimaries(db);

    for (let connection of connectionsToPrimaries.concat(FixtureHelpers.getSecondaries(db))) {
        cleanupHelloFailInjection(connection);
    }
}

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
