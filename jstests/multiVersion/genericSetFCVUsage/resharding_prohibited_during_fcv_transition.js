/**
 * Tests that a fresh resharding operation cannot be started while an FCV transition is in
 * progress. During the enableTargetFeatures/commitAddedFeatures phases the in-memory FCV reads
 * as the fully upgraded/downgraded target version even though the transition has not completed on
 * all shards, so the resharding start guard must also reject while the FCV document still carries
 * a transition phase (SERVER-131380, BF-44533).
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, config: 1});
const configPrimary = st.configRS.getPrimary();

const dbName = "testDB";
const collName = "testColl";
const ns = dbName + "." + collName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

function runCase(fromFCV, toFCV) {
    jsTest.log("Running FCV transition case from " + fromFCV + " to " + toFCV);

    assert.commandWorked(
        st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: fromFCV, confirm: true}),
    );

    assert(st.s.getDB(dbName).getCollection(collName).drop());
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {a: 1}}));

    // Pause setFCV after the in-memory FCV has flipped to the target version but before the final
    // write that clears the persisted transition phase.
    const fp = configureFailPoint(configPrimary, "hangBeforeFinalizingFCV");
    const fcvThread = new Thread(
        function (host, version) {
            const conn = new Mongo(host);
            assert.commandWorked(
                conn
                    .getDB("admin")
                    .runCommand({setFeatureCompatibilityVersion: version, confirm: true}),
            );
        },
        st.s.host,
        toFCV,
    );
    fcvThread.start();
    fp.wait();

    // The persisted phase is set (transition in progress), so a fresh resharding start is rejected
    // even though the in-memory FCV already reads as the target version.
    assert.commandFailedWithCode(
        st.s.adminCommand({reshardCollection: ns, key: {b: 1}, numInitialChunks: 1}),
        ErrorCodes.CommandNotSupported,
    );

    fp.off();
    fcvThread.join();

    // Once the transition has completed, resharding is allowed again.
    assert.commandWorked(
        st.s.adminCommand({reshardCollection: ns, key: {b: 1}, numInitialChunks: 1}),
    );
}

// Downgrade direction.
runCase(latestFCV, lastLTSFCV);
// Upgrade direction.
runCase(lastLTSFCV, latestFCV);

st.stop();
