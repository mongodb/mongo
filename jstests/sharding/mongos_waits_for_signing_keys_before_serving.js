/**
 * Explicitly verifies that a mongos does not begin accepting client connections until clusterTime
 * signing keys are available on the config server. Until keys are available, mongos cannot validate
 * or sign clusterTime and therefore cannot uphold causal-consistency / afterClusterTime read-concern
 * guarantees, so it must refuse to serve requests.
 *
 * This is enforced during startup by 'waitForSigningKeys', which blocks initialization (before the
 * transport layer starts accepting connections) until the key manager has seen keys. This behavior
 * was previously only exercised as a side effect of causal-consistency passthrough suites; this
 * test asserts it directly.
 *
 * This test restarts the config server replica set and relies on its replica set config and data
 * persisting across that restart, so it requires a persistent storage engine.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_persistence,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Bring up a config replica set. Initiation generates signing keys (and the test harness waits for
// them), so we let that happen first, then stop key generation and remove the keys to simulate a
// config server that has no keys available for a starting mongos to obtain.
const configRS = new ReplSetTest({name: "configRS", nodes: 1, useHostName: true});
configRS.startSet({configsvr: ""});
const replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
configRS.initiate(replConfig);

const configPrimary = configRS.getPrimary();
assert.commandWorked(
    configPrimary.adminCommand({configureFailPoint: "disableKeyGeneration", mode: "alwaysOn"}),
);
configPrimary.getDB("admin").system.keys.remove({purpose: "HMAC"});
assert.eq(
    0,
    configPrimary.getDB("admin").system.keys.count({purpose: "HMAC"}),
    "expected all signing keys to be removed from the config server",
);

// Start a mongos while no keys exist. It must block in startup (waitForSigningKeys) and must not
// accept connections.
const mongos = MongoRunner.runMongos({configdb: configRS.getURL(), waitForConnect: false});

try {
    // The mongos must refuse connections for as long as keys are unavailable. Verify it stays
    // unreachable across several attempts rather than racing on a single check.
    for (let i = 0; i < 5; i++) {
        assert.throws(
            () => new Mongo(mongos.host),
            [],
            "mongos accepted a connection before signing keys were available",
        );
        sleep(1000);
    }

    // Restart the config server with key generation allowed again. The restart is what makes keys
    // regenerate promptly: the key manager's refresh loop runs a generation pass immediately on
    // startup, whereas the previously running task was sleeping until near key expiry and would not
    // pick up the failpoint being cleared.
    configRS.restart(0);
    configRS.awaitNodesAgreeOnPrimary();

    // Once keys exist, mongos finishes startup and begins accepting connections.
    let conn;
    assert.soon(
        () => {
            try {
                conn = new Mongo(mongos.host);
                return true;
            } catch (e) {
                return false;
            }
        },
        "mongos did not begin accepting connections after signing keys became available",
        60 * 1000,
    );
    assert.commandWorked(conn.getDB("admin").runCommand({ping: 1}));
} finally {
    MongoRunner.stopMongos(mongos);
    configRS.stopSet();
}
