/**
 * Verifies that the successful execution of setFCV generates the expected op log entries across the shards of the cluster.
 * TODO (SERVER-98118): Delete this file once featureFlagChangeStreamPreciseShardTargeting reaches last-lts.
 * @tags: [
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_fcv_90,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}, config: 1, configShard: true});

function checkNotificationPresenceAcrossShards(
    numNotificationsByShard,
    lowerBoundClusterTime,
    upperBoundClusterTime,
) {
    const matchingCriteriaForRetargetingOplogEntry = {
        op: "n",
        ns: "",
        o: {msg: {namespacePlacementChanged: ""}},
        "o2.namespacePlacementChanged": 1,
        "o2.ns": {},
        "o2.committedAt": {
            $gt: lowerBoundClusterTime,
            $lte: upperBoundClusterTime,
        },
        ts: {
            $gt: lowerBoundClusterTime,
            $lte: upperBoundClusterTime,
        },
    };

    [st.rs0, st.rs1].forEach((shardRS) => {
        const oplogColl = shardRS.getPrimary().getDB("local").oplog.rs;
        assert.eq(
            oplogColl.countDocuments(matchingCriteriaForRetargetingOplogEntry),
            numNotificationsByShard,
            `Expected to find exactly ${numNotificationsByShard} retargeting oplog entries on shard primary ${shardRS.getURL()}  within the (${tojson(lowerBoundClusterTime)}, ${tojson(upperBoundClusterTime)}] period.`,
        );
    });
}

jsTest.log.info(
    'An FCV downgrade transition triggers one "retargeting" change stream event on each shard.',
);
const clusterTimeBeforeFCVDowngrade = st.s.getDB("admin").runCommand({hello: 1})
    .$clusterTime.clusterTime;
const clusterTimeAfterFCVDowngrade = assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
).$clusterTime.clusterTime;
checkNotificationPresenceAcrossShards(
    1,
    clusterTimeBeforeFCVDowngrade,
    clusterTimeAfterFCVDowngrade,
);

jsTest.log.info(
    'setFCV downgrade generates no "retargeting" change stream events when resolves into a no-op.',
);
const clusterTimeAfterNoopFCVDowngrade = assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
).$clusterTime.clusterTime;
checkNotificationPresenceAcrossShards(
    0,
    clusterTimeAfterFCVDowngrade,
    clusterTimeAfterNoopFCVDowngrade,
);

jsTest.log.info(
    'An FCV upgrade transition triggers one "retargeting" change stream event on each shard.',
);
const clusterTimeBeforeFCVUpgrade = st.s.getDB("admin").runCommand({hello: 1})
    .$clusterTime.clusterTime;
const clusterTimeAfterFCVUpgrade = assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
).$clusterTime.clusterTime;
checkNotificationPresenceAcrossShards(1, clusterTimeBeforeFCVUpgrade, clusterTimeAfterFCVUpgrade);

/*
 * In non-symmetric FCV configurations, a no-op FCV upgrade generates a "retargeting" change
 * stream event on each shard. With symmetric FCV upgrade and downgrade behaves in the same
 * way, hence no "retargeting" change events when setFCV resolves into a no-op.
 */
const expectedNumRetargetingNotificationsForNoopFCVUpgrade = FeatureFlagUtil.isPresentAndEnabled(
    st.s.getDB("admin"),
    "SymmetricFCV",
)
    ? 0
    : 1;

jsTest.log.info(
    'non-symmetric setFCV upgrade generates a "retargeting" change stream event when resolves into a no-op, and none for a symmetric setFCV upgrade',
);
const clusterTimeAfterNoopFCVUpgrade = assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
).$clusterTime.clusterTime;
checkNotificationPresenceAcrossShards(
    expectedNumRetargetingNotificationsForNoopFCVUpgrade,
    clusterTimeAfterFCVUpgrade,
    clusterTimeAfterNoopFCVUpgrade,
);

st.stop();
