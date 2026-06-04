/**
 * Test to make sure that the abort command interrupts a timeseries resharding operation that has
 * not yet persisted a decision.
 *
 * Timeseries variant of resharding_abort_command.js.
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_80,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const originalCollectionNs = "reshardingDb.coll";
const enterAbortFailpointName = "reshardingPauseCoordinatorBeforeStartingErrorFlow";

const nodeTypeEnum = {
    COORDINATOR: 1,
    DONOR: 2,
    RECIPIENT: 3,
    NO_EXTRA_FAILPOINTS_SENTINEL: 4,
};

const abortLocationEnum = {
    BEFORE_STEADY_STATE: 1,
    BEFORE_DECISION_PERSISTED: 2,
    AFTER_DECISION_PERSISTED: 3,
};

let getConnStringsFromNodeType = (nodeType, reshardingTest, topology) => {
    let connStrings = [];
    if (nodeType == nodeTypeEnum.COORDINATOR) {
        connStrings.push(topology.configsvr.nodes[0]);
    } else if (nodeType == nodeTypeEnum.DONOR) {
        for (let donor of reshardingTest.donorShardNames) {
            connStrings.push(topology.shards[donor].primary);
        }
    } else if (nodeType == nodeTypeEnum.RECIPIENT) {
        for (let recipient of reshardingTest.recipientShardNames) {
            connStrings.push(topology.shards[recipient].primary);
        }
    } else if (nodeType == nodeTypeEnum.NO_EXTRA_FAILPOINTS_SENTINEL) {
        //
    } else {
        throw "unsupported node type in resharding abort test";
    }

    return connStrings;
};

let getMongosFromConnStrings = (connStrings) => {
    let mongos = [];
    for (let conn of connStrings) {
        mongos.push(new Mongo(conn));
    }
    return mongos;
};

let generateFailpoints = (failpointName, failpointNodeType, reshardingTest, topology, failpointMode = "alwaysOn") => {
    const failpointTargetConnStrings = getConnStringsFromNodeType(failpointNodeType, reshardingTest, topology);
    const failpointHosts = getMongosFromConnStrings(failpointTargetConnStrings);

    let failpoints = [];
    for (let host of failpointHosts) {
        failpoints.push(configureFailPoint(host, failpointName, {} /* data */, failpointMode));
    }

    return failpoints;
};

let generateAbortThread = (mongosConnString, ns, expectedErrorCodes) => {
    return new Thread(
        (mongosConnString, ns, expectedErrorCodes) => {
            const mongos = new Mongo(mongosConnString);
            if (expectedErrorCodes == ErrorCodes.OK) {
                assert.commandWorked(mongos.adminCommand({abortReshardCollection: ns}));
            } else {
                assert.commandFailedWithCode(mongos.adminCommand({abortReshardCollection: ns}), expectedErrorCodes);
            }
        },
        mongosConnString,
        ns,
        expectedErrorCodes,
    );
};

let triggerAbortAndCoordinateFailpoints = (
    failpointName,
    failpointNodeType,
    reshardingTest,
    topology,
    mongos,
    configsvr,
    abortThread,
    failpoints,
    executeBeforeWaitingOnFailpointsFn,
    executeAfterWaitingOnFailpointsFn,
    executeAfterAbortingFn,
) => {
    if (executeBeforeWaitingOnFailpointsFn) {
        jsTestLog(`Executing the before-waiting-on-failpoint function`);
        executeBeforeWaitingOnFailpointsFn(mongos, originalCollectionNs);
    }

    if (failpointNodeType != nodeTypeEnum.NO_EXTRA_FAILPOINTS_SENTINEL) {
        jsTestLog(`Wait for the failpoint ${failpointName} to be reached on all applicable nodes`);
        for (let failpoint of failpoints) {
            failpoint.wait();
        }
    }

    if (executeAfterWaitingOnFailpointsFn) {
        jsTestLog(`Executing the after-waiting-on-failpoint function`);
        executeAfterWaitingOnFailpointsFn(reshardingTest, topology, mongos, originalCollectionNs);
    }

    jsTestLog(`Wait for the coordinator to recognize that it's been aborted`);

    const enterAbortFailpoint = configureFailPoint(configsvr, enterAbortFailpointName);
    abortThread.start();
    enterAbortFailpoint.wait();

    if (executeAfterAbortingFn) {
        jsTestLog(`Executing the after-aborting function`);
        executeAfterAbortingFn(reshardingTest, topology, mongos, originalCollectionNs);
    }

    enterAbortFailpoint.off();

    if (failpointNodeType != nodeTypeEnum.NO_EXTRA_FAILPOINTS_SENTINEL) {
        jsTestLog(
            `Turn off the failpoint ${failpointName} to allow both the abort and the resharding operation to complete`,
        );
        for (let failpoint of failpoints) {
            failpoint.off();
        }
    }
};

let triggerPostDecisionPersistedAbort = (mongos, abortThread) => {
    const [dbName, ...collParts] = originalCollectionNs.split(".");
    const opNs = getTimeseriesCollForDDLOps(
        mongos.getDB(dbName),
        mongos.getDB(dbName).getCollection(collParts.join(".")),
    ).getFullName();
    assert.soon(() => {
        const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({ns: opNs});
        return (
            coordinatorDoc == null || coordinatorDoc.state === "decision-persisted" || coordinatorDoc.state === "done"
        );
    });

    abortThread.start();
};

const runAbortWithFailpoint = (
    failpointName,
    failpointNodeType,
    abortLocation,
    {
        executeBeforeReshardingStartsFn = null,
        executeAtStartOfReshardingFn = null,
        executeBeforeWaitingOnFailpointsFn = null,
        executeAfterWaitingOnFailpointsFn = null,
        executeAfterAbortingFn = null,
    } = {},
) => {
    const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const recipientShardNames = reshardingTest.recipientShardNames;

    const timeseriesInfo = {timeField: "ts", metaField: "metaTest"};

    const sourceCollection = reshardingTest.createShardedCollection({
        ns: originalCollectionNs,
        // "metaTest.x" is the user-facing field; translated internally to {"meta.x": 1}.
        shardKeyPattern: {"metaTest.x": 1},
        chunks: [
            {min: {"meta.x": MinKey}, max: {"meta.x": 0}, shard: donorShardNames[0]},
            {min: {"meta.x": 0}, max: {"meta.x": MaxKey}, shard: donorShardNames[1]},
        ],
        shardCollOptions: {timeseries: timeseriesInfo},
    });

    const mongos = sourceCollection.getMongo();
    const topology = DiscoverTopology.findConnectedNodes(mongos);
    const configsvr = new Mongo(topology.configsvr.nodes[0]);

    let expectedAbortErrorCodes = ErrorCodes.OK;
    let expectedReshardingErrorCode = ErrorCodes.ReshardCollectionAborted;

    // If the abort is going to happen after the decision is persisted, it's expected that the
    // resharding operation will have finished without error, and that the abort itself will
    // error.
    if (abortLocation == abortLocationEnum.AFTER_DECISION_PERSISTED) {
        expectedAbortErrorCodes = [ErrorCodes.ReshardCollectionCommitted, ErrorCodes.NoSuchReshardCollection];
        expectedReshardingErrorCode = ErrorCodes.OK;
    }

    // abortReshardCollection does not translate user-facing ns to bucket ns like reshardCollection
    // does, so in legacy timeseries (FCV < 9.0) we must pass the bucket namespace explicitly.
    const abortNs = getTimeseriesCollForDDLOps(
        mongos.getDB("reshardingDb"),
        mongos.getDB("reshardingDb").getCollection("coll"),
    ).getFullName();
    const abortThread = generateAbortThread(topology.mongos.nodes[0], abortNs, expectedAbortErrorCodes);

    if (executeBeforeReshardingStartsFn) {
        jsTestLog(`Executing the before-resharding-starts fn`);
        executeBeforeReshardingStartsFn(reshardingTest, topology, mongos, originalCollectionNs);
    }

    let failpoints = [];
    if (failpointNodeType != nodeTypeEnum.NO_EXTRA_FAILPOINTS_SENTINEL) {
        failpoints = generateFailpoints(failpointName, failpointNodeType, reshardingTest, topology);
    }

    reshardingTest.withReshardingInBackground(
        {
            // "metaTest.y" is the user-facing field; translated internally to {"meta.y": 1}.
            newShardKeyPattern: {"metaTest.y": 1},
            newChunks: [
                {min: {"meta.y": MinKey}, max: {"meta.y": 0}, shard: recipientShardNames[0]},
                {min: {"meta.y": 0}, max: {"meta.y": MaxKey}, shard: recipientShardNames[1]},
            ],
        },
        () => {
            if (executeAtStartOfReshardingFn) {
                jsTestLog(`Executing the start-of-resharding fn`);
                executeAtStartOfReshardingFn(reshardingTest, topology, mongos, originalCollectionNs);
            }

            if (abortLocation == abortLocationEnum.BEFORE_STEADY_STATE) {
                triggerAbortAndCoordinateFailpoints(
                    failpointName,
                    failpointNodeType,
                    reshardingTest,
                    topology,
                    mongos,
                    configsvr,
                    abortThread,
                    failpoints,
                    executeBeforeWaitingOnFailpointsFn,
                    executeAfterWaitingOnFailpointsFn,
                    executeAfterAbortingFn,
                );
            }
        },
        {
            expectedErrorCode: expectedReshardingErrorCode,
            postCheckConsistencyFn: () => {
                if (abortLocation == abortLocationEnum.BEFORE_DECISION_PERSISTED) {
                    triggerAbortAndCoordinateFailpoints(
                        failpointName,
                        failpointNodeType,
                        reshardingTest,
                        topology,
                        mongos,
                        configsvr,
                        abortThread,
                        failpoints,
                        executeBeforeWaitingOnFailpointsFn,
                        executeAfterWaitingOnFailpointsFn,
                        executeAfterAbortingFn,
                    );
                }
            },
            postDecisionPersistedFn: () => {
                if (abortLocation == abortLocationEnum.AFTER_DECISION_PERSISTED) {
                    triggerPostDecisionPersistedAbort(mongos, abortThread);
                }
            },
        },
    );

    // Verify that the shard key in config.collections uses the internal field name (meta.*),
    // not the user-facing metaField name (metaTest.*), and reflects the outcome of the operation.
    // config.collections is keyed by the bucket namespace for viewful timeseries (FCV < 9.0) and
    // by the user-facing namespace for viewless timeseries (FCV >= 9.0).
    const configNs = getTimeseriesCollForDDLOps(
        mongos.getDB("reshardingDb"),
        mongos.getDB("reshardingDb").getCollection("coll"),
    ).getFullName();
    const collEntry = mongos.getCollection("config.collections").findOne({_id: configNs});
    assert.neq(null, collEntry);
    const expectedKey = expectedReshardingErrorCode == ErrorCodes.OK ? {"meta.y": 1} : {"meta.x": 1};
    assert.docEq(expectedKey, collEntry.key);

    const reshardingMetrics = configsvr.getDB("admin").serverStatus({}).shardingStatistics.resharding;

    let reshardingOperationsFinalCount = reshardingMetrics.countStarted;
    let reshardingSuccessesFinalCount = reshardingMetrics.countSucceeded;
    let reshardingCanceledFinalCount = reshardingMetrics.countCanceled;

    assert.eq(reshardingOperationsFinalCount, 1);

    if (expectedReshardingErrorCode == ErrorCodes.OK) {
        assert.eq(reshardingSuccessesFinalCount, 1);
        assert.eq(reshardingCanceledFinalCount, 0);
    } else if (expectedAbortErrorCodes == ErrorCodes.OK) {
        assert.eq(reshardingCanceledFinalCount, 1);
        assert.eq(reshardingSuccessesFinalCount, 0);
    }

    abortThread.join();
    reshardingTest.teardown();
};

runAbortWithFailpoint(
    "reshardingPauseRecipientBeforeCloning",
    nodeTypeEnum.RECIPIENT,
    abortLocationEnum.BEFORE_STEADY_STATE,
);

runAbortWithFailpoint(
    "reshardingPauseRecipientDuringCloning",
    nodeTypeEnum.RECIPIENT,
    abortLocationEnum.BEFORE_STEADY_STATE,
);

runAbortWithFailpoint(
    "reshardingPauseRecipientDuringOplogApplication",
    nodeTypeEnum.RECIPIENT,
    abortLocationEnum.BEFORE_STEADY_STATE,
    {
        executeAfterWaitingOnFailpointsFn: (reshardingTest, topology, mongos, ns) => {
            assert.commandWorked(
                mongos.getCollection(ns).insert([
                    {data: 0, ts: new Date(), metaTest: {x: -10, y: -10}},
                    {data: 1, ts: new Date(), metaTest: {x: 10, y: -10}},
                    {data: 2, ts: new Date(), metaTest: {x: -10, y: 10}},
                    {data: 3, ts: new Date(), metaTest: {x: 10, y: 10}},
                ]),
            );
        },
    },
);

function waitForAllRecipientsToReachApplying(mongos, ns) {
    // In legacy timeseries (FCV < 9.0), config.reshardingOperations uses the bucket namespace.
    const [dbName, ...collParts] = ns.split(".");
    const opNs = getTimeseriesCollForDDLOps(
        mongos.getDB(dbName),
        mongos.getDB(dbName).getCollection(collParts.join(".")),
    ).getFullName();
    assert.soon(() => {
        const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({ns: opNs});

        if (
            coordinatorDoc === null ||
            !Array.isArray(coordinatorDoc.recipientShards) ||
            coordinatorDoc.recipientShards.length === 0
        ) {
            return false;
        }

        for (const shardEntry of coordinatorDoc.recipientShards) {
            if (shardEntry.mutableState.state !== "applying") {
                return false;
            }
        }

        return true;
    });
}

// Rely on the resharding_test_fixture's built-in failpoint that hangs before switching to
// the blocking writes state.
runAbortWithFailpoint(null, nodeTypeEnum.NO_EXTRA_FAILPOINTS_SENTINEL, abortLocationEnum.BEFORE_STEADY_STATE, {
    executeAtStartOfReshardingFn: (reshardingTest, topology, mongos, ns) => {
        waitForAllRecipientsToReachApplying(mongos, ns);
    },
});

// Test that the resharding operation can successfully be aborted even when the commit monitor won't
// ever signal to the coordinator the resharding operation is ready to commit.
runAbortWithFailpoint("hangBeforeQueryingRecipients", nodeTypeEnum.COORDINATOR, abortLocationEnum.BEFORE_STEADY_STATE, {
    executeAtStartOfReshardingFn: (reshardingTest, topology, mongos, ns) => {
        waitForAllRecipientsToReachApplying(mongos, ns);
    },
});

runAbortWithFailpoint(null, nodeTypeEnum.NO_EXTRA_FAILPOINTS_SENTINEL, abortLocationEnum.AFTER_DECISION_PERSISTED);

// The resharding test fixture uses its own set of coordinator failpoints for resharding
// checkpoints. It may not be possible to insert documents once the second checkpoint is reached.
// Because of this, we cannot rely on the failpoint mechanism set up in this test file. Instead, we
// must manually activate and unactivate the failpoints across a checkpoint threshold.
//
// executeAtStartOfReshardingFn runs while the coordinator is in steady state (checkpoint 1), and
// executeAfterWaitingOnFailpointsFn will run while the coordinator is blocking writes (checkpoint
// 2).

let recipientFailpoints = [];
runAbortWithFailpoint(null, nodeTypeEnum.NO_EXTRA_FAILPOINTS_SENTINEL, abortLocationEnum.BEFORE_DECISION_PERSISTED, {
    executeBeforeReshardingStartsFn: (reshardingTest, topology) => {
        recipientFailpoints = generateFailpoints(
            "reshardingPauseRecipientDuringOplogApplication",
            nodeTypeEnum.RECIPIENT,
            reshardingTest,
            topology,
        );
    },
    executeAtStartOfReshardingFn: (reshardingTest, topology, mongos, ns) => {
        for (let failpoint of recipientFailpoints) {
            failpoint.wait();
        }
        assert.commandWorked(
            mongos.getCollection(ns).insert([
                {data: 4, ts: new Date(), metaTest: {x: -10, y: -10}},
                {data: 5, ts: new Date(), metaTest: {x: 10, y: -10}},
                {data: 6, ts: new Date(), metaTest: {x: -10, y: 10}},
                {data: 7, ts: new Date(), metaTest: {x: 10, y: 10}},
            ]),
        );
    },
    executeAfterWaitingOnFailpointsFn: (reshardingTest, topology, mongos, ns) => {
        // config.localReshardingOperations stores docs under the bucket ns in legacy timeseries.
        const localNs = getTimeseriesCollForDDLOps(
            mongos.getDB("reshardingDb"),
            mongos.getDB("reshardingDb").getCollection("coll"),
        ).getFullName();
        assert.soon(() => {
            for (let donor of reshardingTest.donorShardNames) {
                const donorConn = new Mongo(topology.shards[donor].primary);
                const donorDoc = donorConn.getCollection("config.localReshardingOperations.donor").findOne({
                    ns: localNs,
                });
                return donorDoc != null && donorDoc.mutableState.state === "blocking-writes";
            }
        });
    },
    executeAfterAbortingFn: () => {
        for (let failpoint of recipientFailpoints) {
            failpoint.off();
        }
    },
});
