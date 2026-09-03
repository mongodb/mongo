/**
 * Tests that missing sharding metadata indexes on non-empty config collections cause fatal
 * assertions during step up.
 *
 * Additionally, checks that each index is printed in the logs when more than one index is missing
 * and unfixable due to the collection being non-empty.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test intentionally triggers a tassert in createIndexesOnCollectionAtStepUp. Disable the
// behavior of aborting on exit so that we can continue the test and stop the sharded cluster
// normally.
TestData.testingDiagnosticsEnabled = false;

// This test triggers an unclean shutdown (an fassert), which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;
TestData.cleanUpCoreDumpsFromExpectedCrash = true;

const st = new ShardingTest({
    shards: 1,
    config: 2,
    configShard: true,
    initiateWithDefaultElectionTimeout: true,
});

const configRS = st.configRS;
const configPrimary = configRS.getPrimary();
const configSecondary = configRS.getSecondary();
const secondaryNodeId = configSecondary.nodeId;

// dropIndex requires a writable node. Drop on the current primary so the change replicates to the
// secondary that will step up.
assert.commandWorked(configPrimary.getDB("config").shards.dropIndex("uuid_1"));
assert.commandWorked(configPrimary.getDB("config").shards.dropIndex("host_1"));
configRS.awaitReplication();

assert.eq(
    configSecondary
        .getDB("config")
        .shards.getIndexes()
        .some((idx) => idx.key.uuid !== undefined),
    false,
    "expected uuid index to be dropped on config secondary before step up",
);

assert.eq(
    configSecondary
        .getDB("config")
        .shards.getIndexes()
        .some((idx) => idx.key.host !== undefined),
    false,
    "expected host index to be dropped on config secondary before step up",
);

// Step up without the bypass parameter should trip a tassert and then fassert during config
// initialization. The replSetStepUp command itself succeeds; the node aborts asynchronously while
// finishing onStepUpComplete.
clearRawMongoProgramOutput();
assert.commandWorked(configSecondary.adminCommand({replSetStepUp: 1}));
assert.eq(MongoRunner.EXIT_ABORT, waitMongoProgram(configSecondary.port));
assert.soon(function () {
    const output = rawMongoProgramOutput(".*");
    return (
        output.search(/Tripwire assertion.*12352501/) >= 0 &&
        output.search(/Tripwire assertion.*Indexes:.*host_1/) >= 0 &&
        output.search(/Tripwire assertion.*Indexes:.*uuid_1/) >= 0 &&
        output.search(/Fatal assertion.*40184/) >= 0
    );
}, "expected a tripwire for each missing index and an fassert when stepping up without bypass");

// While the crashed node is down, the surviving member cannot win an election on its own. Before
// bringing the secondary back, freeze it so it cannot steal the election once both nodes are up
// again. The survivor also lacks uuid_1, so stepping up without the bypass would hit the same
// fassert.
assert.commandWorked(configPrimary.adminCommand({replSetFreeze: 120}));

// The node already aborted; start it back up without calling stop(). Pass the bypass parameter at
// startup so it is enabled before the node can auto-elect as primary.
configRS.start(
    secondaryNodeId,
    {
        setParameter: {
            allowDeferredInternalCatalogIndexBuildOnNonEmptyCollectionDuringStepUp: true,
        },
    },
    true,
);
configRS.awaitSecondaryNodes();

const restartedSecondary = configRS.nodes[secondaryNodeId];

// With the bypass parameter, step up should succeed without building the missing indexes.
configRS.stepUp(restartedSecondary);
configRS.awaitReplication();

const newPrimary = configRS.getPrimary();
assert.eq(newPrimary.host, restartedSecondary.host, "expected config secondary to become primary");

// The bypass doesn't create the indexes, just allows the node to step up ignoring the index build
// altogether.
assert.eq(
    newPrimary
        .getDB("config")
        .shards.getIndexes()
        .some((idx) => idx.key.uuid !== undefined),
    false,
    "expected uuid index to be missing on config primary after step up",
);
assert.eq(
    newPrimary
        .getDB("config")
        .shards.getIndexes()
        .some((idx) => idx.key.host !== undefined),
    false,
    "expected host index to be missing on config primary after step up",
);

st.stop();
