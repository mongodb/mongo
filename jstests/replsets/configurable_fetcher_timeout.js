/**
 * Tests that the fetcher timeout when selecting a sync source candidate is configurable. This enables customers
 * to be able to quickly retry selecting a sync source candidate if one sync source is timing out in its
 * response.
 *
 * @tags: [requires_mongobridge]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({name: jsTestName(), nodes: 3, settings: {chainingAllowed: false}, useBridge: true});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const kFetcherTimeoutMillis = 15000;
assert.gt(
    kFetcherTimeoutMillis,
    rst.getReplSetConfigFromNode().settings.electionTimeoutMillis,
    "fetcher timeout was not greater than election timeout",
);
for (let node of rst.nodes) {
    assert.commandWorked(
        node.adminCommand({setParameter: 1, syncSourceResolverFindFetcherTimeoutMillis: kFetcherTimeoutMillis}),
    );
}

const originalPrimary = rst.getPrimary();
rst.awaitReplication();
rst.awaitSecondaryNodes();

const secondary1 = rst.getSecondaries()[0];
const secondary2 = rst.getSecondaries()[1];

jsTestLog("Stoping replication on all secondaries");
let stopReplProducer1 = configureFailPoint(secondary1, "stopReplProducer");
let stopReplProducer2 = configureFailPoint(secondary2, "stopReplProducer");

stopReplProducer1.wait();
stopReplProducer2.wait();

assert.commandWorked(
    originalPrimary.adminCommand({
        configureFailPoint: "hangBeforeFetcherFindCommandOnOplog",
        data: {nss: "local.oplog.rs", shouldCheckForInterrupt: true},
        mode: "alwaysOn",
    }),
);

const start = Date.now();
jsTestLog("Starting time: " + start);

jsTestLog("Turn off oplog fetcher failpoint on secondaries");
stopReplProducer1.off();
stopReplProducer2.off();

jsTestLog("Disconnect 1 secondary from primary so election triggers");
originalPrimary.disconnect(secondary1);

jsTestLog("Waiting for the new primary to step up");
let newPrimary;
assert.soon(() => {
    newPrimary = rst.getPrimary();
    return originalPrimary.host != newPrimary.host;
});

jsTestLog("Unblock old primary from secondary");
originalPrimary.reconnect(secondary1);

jsTestLog("Waiting for secondaries to time out on find command");
checkLog.containsJson(secondary1, 5579707); /* Fetcher return error log message */
checkLog.containsJson(secondary2, 5579707); /* Fetcher return error log message */

jsTestLog("Secondaries have timed out, turning off failpoint to hang");
assert.commandWorked(
    originalPrimary.adminCommand({
        configureFailPoint: "hangBeforeFetcherFindCommandOnOplog",
        mode: "off",
    }),
);

const end = Date.now();
jsTestLog("Ending time: " + end);

const totalLag = end - start;
jsTestLog("Total lag time: " + totalLag + " ms");

// Verify that the total lag is within the bounds of the fetcher timeout.
assert.gte(totalLag, kFetcherTimeoutMillis);
assert.lte(totalLag, 2 * kFetcherTimeoutMillis);

rst.awaitSecondaryNodes();
rst.stopSet();
