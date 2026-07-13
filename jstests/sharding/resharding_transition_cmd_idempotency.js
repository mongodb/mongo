/**
 * Tests idempotency of resharding transition commands.
 *
 * @tags: [
 *   requires_fcv_90,
 *   uses_resharding,
 *   featureFlagReshardingInitNoRefresh,
 *   featureFlagReshardingCloneNoRefresh,
 *   featureFlagReshardingNoRefreshApplyingAndBlockingWrites,
 *   featureFlagReshardingSkipCloningAndApplyingIfApplicable,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 1},
    other: {
        configOptions: {setParameter: {reshardingMinimumOperationDurationMillis: 0}},
        rsOptions: {setParameter: {reshardingMinimumOperationDurationMillis: 0}},
    },
});

const dbName = "reshardingDb";
const collName = "coll";
const ns = dbName + "." + collName;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {oldKey: 1}}));
assert.commandWorked(
    st.s.getCollection(ns).insert([
        {oldKey: 1, newKey: -1},
        {oldKey: 2, newKey: -2},
    ]),
);

const donorPrimary = st.rs0.getPrimary();
const recipientPrimary = st.rs1.getPrimary();

const initializeFp = configureFailPoint(
    recipientPrimary,
    "errorAfterProcessingReshardRecipientInitializeCommand",
);
const recipientCloneFp = configureFailPoint(
    recipientPrimary,
    "errorAfterProcessingReshardRecipientCloneCommand",
);
const doneCloningFp = configureFailPoint(
    donorPrimary,
    "errorAfterProcessingReshardDonorRecipientsFinishedCloningCommand",
);
const donorCriticalSectionFp = configureFailPoint(
    donorPrimary,
    "errorAfterProcessingReshardDonorCriticalSectionStartedCommand",
);
const recipientCriticalSectionFp = configureFailPoint(
    recipientPrimary,
    "errorAfterProcessingReshardRecipientCriticalSectionStartedCommand",
);

const reshardThread = new Thread(
    (host, ns, recipientShardName) => {
        const mongos = new Mongo(host);
        assert.commandWorked(
            mongos.adminCommand({
                reshardCollection: ns,
                key: {newKey: 1},
                _presetReshardedChunks: [
                    {
                        recipientShardId: recipientShardName,
                        min: {newKey: MinKey},
                        max: {newKey: MaxKey},
                    },
                ],
            }),
        );
    },
    st.s.host,
    ns,
    st.shard1.shardName,
);
reshardThread.start();

jsTest.log.info("Waiting for the initialize failpoint to be hit");
initializeFp.wait();
initializeFp.off();

jsTest.log.info("Waiting for the recipient clone failpoint to be hit");
recipientCloneFp.wait();
recipientCloneFp.off();

jsTest.log.info("Waiting for the donor doneCloning failpoint to be hit");
doneCloningFp.wait();
doneCloningFp.off();

jsTest.log.info("Waiting for the donor critical section failpoint to be hit");
donorCriticalSectionFp.wait();
donorCriticalSectionFp.off();

jsTest.log.info("Waiting for the recipient critical section failpoint to be hit");
recipientCriticalSectionFp.wait();
recipientCriticalSectionFp.off();

reshardThread.join();

st.stop();
