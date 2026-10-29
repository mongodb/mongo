/**
 * Tests resharding version pinning behavior together with different FCV version.
 * This setup uses embedded config servers to exercise both setFCV logic for
 * participants that are both normal shards and config shards.
 *
 * @tags: [
 *   resource_intensive,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

let mustTestReshardingPauseFPNames = ["reshardingPauseCoordinatorBeforeCloning"];
let otherReshardingPauseFPNames = [
    "reshardingPauseCoordinatorAfterPreparingToDonate",
    // Note: we cannot use pause before initializing because we call getCoordinatorDocWrittenFuture
    // while inside the FixedFCVRegion, which means that the setFCV thread cannot start if this is
    // kept paused.
    // "reshardingPauseCoordinatorBeforeInitializing", // intentionally excluded
    "reshardingPauseCoordinatorBeforeApplying",
    "reshardingPauseCoordinatorBeforeBlockingWrites",
    "reshardingPauseCoordinatorBeforeDecisionPersisted",
    "reshardingPauseBeforeTellingParticipantsToCommit",
];

function assertReshardingFCVConsistency(st, configPrimary, expectedFCVStr) {
    const coordOps = configPrimary
        .getDB("admin")
        .aggregate([
            {$currentOp: {allUsers: true, localOps: true}},
            {$match: {desc: {$regex: "ReshardingMetricsCoordinatorService"}}},
        ])
        .toArray();
    coordOps.forEach((op) => {
        assert.eq(op.versionContext.OFCV, expectedFCVStr, {
            msg: "coordinator versionContext mismatch",
            op,
        });
    });

    [st.shard0, st.shard1].forEach((shard) => {
        const participantOps = shard.rs
            .getPrimary()
            .getDB("admin")
            .aggregate([
                {$currentOp: {allUsers: true, localOps: true}},
                {
                    $match: {
                        desc: {
                            $regex: "ReshardingMetricsRecipientService|ReshardingMetricsDonorService",
                        },
                    },
                },
            ])
            .toArray();
        participantOps.forEach((op) => {
            assert.eq(op.versionContext.OFCV, expectedFCVStr, {
                msg: "participant versionContext mismatch",
                op,
            });
        });
    });
}

function testReshardingWithFCV(st, sourceNs, startingFCVStr, targetFCVStr, reshardingPauseFPName) {
    jsTest.log(`Testing resharding with FCV change from ${startingFCVStr} to ${targetFCVStr} while\
         paused at ${reshardingPauseFPName}`);

    const mongos = st.s;

    assert.commandWorked(
        mongos.adminCommand({setFeatureCompatibilityVersion: startingFCVStr, confirm: true}),
    );

    const donorShardNames = [st.shard0.shardName, st.shard1.shardName];
    const recipientShardNames = donorShardNames;

    const [dbName, collName] = sourceNs.split(".");
    assert.commandWorked(
        mongos.adminCommand({enableSharding: dbName, primaryShard: donorShardNames[0]}),
    );
    assert.commandWorked(mongos.adminCommand({shardCollection: sourceNs, key: {x: 1}}));
    assert.commandWorked(mongos.adminCommand({split: sourceNs, middle: {x: 0}}));
    assert.commandWorked(
        mongos.adminCommand({moveChunk: sourceNs, find: {x: 0}, to: donorShardNames[1]}),
    );
    const sourceCollection = mongos.getDB(dbName).getCollection(collName);
    assert.commandWorked(
        sourceCollection.insertMany([
            {x: -1, y: 1},
            {x: 1, y: -1},
        ]),
    );

    const configPrimary = st.configRS.getPrimary();
    const pauseReshardingFp = configureFailPoint(configPrimary, reshardingPauseFPName);

    const reshardingThread = new Thread(
        function (host, ns, presetChunks) {
            const conn = new Mongo(host);
            const res = conn.adminCommand({
                reshardCollection: ns,
                key: {y: 1},
                _presetReshardedChunks: presetChunks,
            });

            if (!res.ok) {
                assert.eq(
                    res.code,
                    ErrorCodes.ReshardCollectionInterruptedDueToFCVChange,
                    "Resharding failed with unexpected error code",
                );
            }
        },
        mongos.host,
        sourceNs,
        [
            {min: {y: MinKey}, max: {y: 0}, recipientShardId: recipientShardNames[0]},
            {min: {y: 0}, max: {y: MaxKey}, recipientShardId: recipientShardNames[1]},
        ],
    );

    let fcvHangFp;
    let awaitSetFCV;
    try {
        // Note: It is not possible to start a new resharding while fcv is upgrading/downgrading.
        // We also create the FixedVersionContext inside the FixedFCVRegion. This means that it is
        // not possible for FixedVersionContext to have upgrading/downgrading fcv versions.
        reshardingThread.start();

        pauseReshardingFp.wait();

        assertReshardingFCVConsistency(st, configPrimary, startingFCVStr);

        // Hang the FCV change right after the FCV document is written to the kDowngrading
        // transitional state, before abortAllReshardCollection is called.
        fcvHangFp = configureFailPoint(configPrimary, "hangAfterConfigServerChangedFCV");

        awaitSetFCV = startParallelShell(
            funWithArgs(function (version) {
                assert.commandWorked(
                    db.adminCommand({setFeatureCompatibilityVersion: version, confirm: true}),
                );
            }, targetFCVStr),
            mongos.port,
        );

        jsTest.log("Waiting for FCV failpoint to get hit");

        // Wait until the FCV change has entered the hang in order to ensure that setFCV is about
        // ready to abort resharding. However, it is still possible for resharding to complete before
        // the abort is executed, but this is alright since the main point of this test is to ensure
        // that resharding does not get stuck.
        fcvHangFp.wait();

        jsTest.log(
            "Finished waiting for FCV failpoint to get hit, " +
                "now unblocking it to allow FCV to abort resharding",
        );
    } finally {
        // Always release the failpoints and join the background operations, even if an assertion
        // above threw. Otherwise a failed assertion would leave the resharding operation paused at
        // reshardingPauseFPName and the reshardingThread/setFCV shell blocked, which would cause
        // the test to hang, and not report any failures.
        if (fcvHangFp) {
            fcvHangFp.off();
        }
        pauseReshardingFp.off();
        reshardingThread.join();
        if (awaitSetFCV) {
            awaitSetFCV();
        }
    }

    assert(sourceCollection.drop());
}

function selectRandom(array, maxCount) {
    const count = Math.min(maxCount, array.length);
    const shuffled = array.sort(() => Math.random());
    return shuffled.slice(0, count);
}

describe("resharding is pinned to version while FCV is in transitional state", function () {
    let st;
    const baseNamespace = "test.user";
    let nsSuffix = 0;

    let nextTestNS = function () {
        return baseNamespace + nsSuffix++;
    };

    before(function () {
        let setParam = {
            featureFlagAuthoritativeShardsCRUD: 1,
            featureFlagAuthoritativeShardsDDL: 1,
            orphanCleanupDelaySecs: 0,
        };
        st = new ShardingTest({
            shards: 2,
            configShard: true,
            other: {
                configOptions: {setParameter: setParam},
                rsOptions: {setParameter: setParam},
            },
        });
    });

    after(function () {
        st.stop();
    });

    let reshardingFpNamesToTest = [
        ...mustTestReshardingPauseFPNames,
        ...selectRandom(otherReshardingPauseFPNames, 1),
    ];

    reshardingFpNamesToTest.forEach((fpName) => {
        it(`completes resharding while FCV is downgrading, ${fpName}`, function () {
            testReshardingWithFCV(st, nextTestNS(), latestFCV, lastLTSFCV, fpName);
        });
    });

    reshardingFpNamesToTest.forEach((fpName) => {
        it(`completes resharding while FCV is upgrading, ${fpName}`, function () {
            testReshardingWithFCV(st, nextTestNS(), lastLTSFCV, latestFCV, fpName);
        });
    });

    if (lastContinuousFCV !== lastLTSFCV) {
        reshardingFpNamesToTest.forEach((fpName) => {
            it(`participants agree with coordinator on FCV upgrading from lastContinuous, ${fpName}`, function () {
                testReshardingWithFCV(st, nextTestNS(), lastContinuousFCV, latestFCV, fpName);
            });
        });

        reshardingFpNamesToTest.forEach((fpName) => {
            it(`participants agree with coordinator on FCV downgrading to lastContinuous, ${fpName}`, function () {
                testReshardingWithFCV(st, nextTestNS(), latestFCV, lastContinuousFCV, fpName);
            });
        });
    }
});
