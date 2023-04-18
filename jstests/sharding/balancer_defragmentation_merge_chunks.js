/**
 * Test the configureCollectionBalancing command and balancerCollectionStatus command
 *
 * @tags: [
 *  # This test does not support stepdowns of CSRS because of how it uses failpoints
 *  # to control phase transition
 *  does_not_support_stepdowns,
 *  requires_fcv_61,
 * ]
 */

// Cannot run the filtering metadata check on tests that run refineCollectionShardKey.
TestData.skipCheckShardFilteringMetadata = true;

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/sharding/libs/find_chunks_util.js");
load("jstests/sharding/libs/defragmentation_util.js");

Random.setRandomSeed();

const st = new ShardingTest({
    mongos: 1,
    shards: 3,
    other: {
        enableBalancer: true,
        // Set global max chunk size to 1MB
        chunkSize: 1,
        configOptions: {
            setParameter: {
                logComponentVerbosity: tojson({sharding: {verbosity: 2}}),
                chunkDefragmentationThrottlingMS: 0,
                reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000 /* 1 day */
            }
        },
        shardOptions: {
            setParameter: {
                reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000 /* 1 day */
            }
        }
    }
});

// setup the database for the test
assert.commandWorked(st.s.adminCommand({enableSharding: 'db'}));
const db = st.getDB('db');
const collNamePrefix = 'testColl';
let collCounter = 0;

function getNewColl() {
    return db[collNamePrefix + '_' + collCounter++];
}

// Shorten time between balancer rounds for faster initial balancing
configureFailPointForRS(
    st.configRS.nodes, 'overrideBalanceRoundInterval', {intervalMs: 200}, 'alwaysOn');

const targetChunkSizeMB = 2;

function setupCollection() {
    st.stopBalancer();
    const coll = getNewColl();
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {key: 1}}));
    defragmentationUtil.createFragmentedCollection(st.s,
                                                   coll.getFullName(),
                                                   10 /* numChunks */,
                                                   targetChunkSizeMB / 2 /* maxChunkFillMB */,
                                                   0 /* numZones */,
                                                   [32 * 1024, 32 * 1024] /* docSizeBytesRange */,
                                                   1000 /* chunkSpacing */,
                                                   false /* disableCollectionBalancing */);
    jsTest.log("Collection " + coll.getFullName() + ", number of chunks before defragmentation: " +
               findChunksUtil.countChunksForNs(st.s.getDB('config'), coll.getFullName()));
    return coll;
}

// Setup collection for first tests
const coll1 = setupCollection();
const coll1Name = coll1.getFullName();

jsTest.log("Test command chunk size bounds.");
{
    st.stopBalancer();
    // 1GB is a valid chunk size.
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll1Name,
        chunkSize: 1024,
    }));
    // This overflows conversion to bytes in an int32_t, ensure it fails non-silently
    assert.commandFailedWithCode(st.s.adminCommand({
        configureCollectionBalancing: coll1Name,
        chunkSize: 4 * 1024 * 1024,
    }),
                                 ErrorCodes.InvalidOptions);
    // This overflows conversion to bytes in an int64_t, ensure it fails non-silently
    assert.commandFailedWithCode(st.s.adminCommand({
        configureCollectionBalancing: coll1Name,
        chunkSize: 8796093022209,
    }),
                                 ErrorCodes.InvalidOptions);
    // Negative numbers are not allowed
    assert.commandFailedWithCode(st.s.adminCommand({
        configureCollectionBalancing: coll1Name,
        chunkSize: -1,
    }),
                                 ErrorCodes.InvalidOptions);
}

jsTest.log("Test command - apply default value to chunkSize");
{
    st.stopBalancer();
    let getMaxChunkSizeFor = (nss) => {
        return st.s.getDB("config").collections.findOne({_id: nss}).maxChunkSizeBytes;
    };

    // Generic Behavior
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll1Name,
        chunkSize: 1024,
    }));
    assert.eq(1024 * 1024 * 1024, getMaxChunkSizeFor(coll1Name));

    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll1Name,
        chunkSize: 0,
    }));
    assert.eq(undefined, getMaxChunkSizeFor(coll1Name));

    // Specific treatment for config.system.sessions
    const sessionsCollName = "config.system.sessions";
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: sessionsCollName,
        chunkSize: 1024,
    }));
    assert.eq(1024 * 1024 * 1024, getMaxChunkSizeFor(sessionsCollName));

    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: sessionsCollName,
        chunkSize: 0,
    }));
    assert.eq(200000, getMaxChunkSizeFor(sessionsCollName));
}

jsTest.log("Begin and end defragmentation with balancer off.");
{
    st.stopBalancer();
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll1Name,
        defragmentCollection: true,
        chunkSize: targetChunkSizeMB,
    }));
    let beforeStatus =
        assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll1Name}));
    assert.eq(beforeStatus.balancerCompliant, false);
    assert.eq(beforeStatus.firstComplianceViolation, 'defragmentingChunks');
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll1Name,
        defragmentCollection: false,
        chunkSize: targetChunkSizeMB,
    }));
    // Phase 3 still has to run
    let afterStatus =
        assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll1Name}));
    assert.eq(afterStatus.balancerCompliant, false);
    assert.eq(afterStatus.firstComplianceViolation, 'defragmentingChunks');
    st.startBalancer();
    defragmentationUtil.waitForEndOfDefragmentation(st.s, coll1Name);
}

jsTest.log("Begin and end defragmentation with balancer on");
{
    st.startBalancer();
    // Allow the first phase transition to build the initial defragmentation state
    let configRSFailPoints = configureFailPointForRS(
        st.configRS.nodes, "skipDefragmentationPhaseTransition", {}, {skip: 1});
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll1Name,
        defragmentCollection: true,
        chunkSize: targetChunkSizeMB,
    }));
    let beforeStatus =
        assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll1Name}));
    assert.eq(beforeStatus.balancerCompliant, false);
    assert.eq(beforeStatus.firstComplianceViolation, 'defragmentingChunks');
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll1Name,
        defragmentCollection: false,
        chunkSize: targetChunkSizeMB,
    }));
    // Ensure that the policy completes the phase transition...
    configRSFailPoints.off();
    defragmentationUtil.waitForEndOfDefragmentation(st.s, coll1Name);
    st.stopBalancer();
}

jsTest.log("Begin defragmentation with balancer off, end with it on");
{
    const coll = setupCollection();
    const nss = coll.getFullName();
    st.stopBalancer();
    // Allow the first phase transition to build the initial defragmentation state
    let configRSFailPoints = configureFailPointForRS(
        st.configRS.nodes, "skipDefragmentationPhaseTransition", {}, {skip: 1});
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: nss,
        defragmentCollection: true,
        chunkSize: targetChunkSizeMB,
    }));
    st.startBalancer();
    let beforeStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: nss}));
    assert.eq(beforeStatus.balancerCompliant, false);
    assert.eq(beforeStatus.firstComplianceViolation, 'defragmentingChunks');
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: nss,
        defragmentCollection: false,
        chunkSize: targetChunkSizeMB,
    }));
    // Ensure that the policy completes the phase transition...
    configRSFailPoints.off();
    defragmentationUtil.waitForEndOfDefragmentation(st.s, nss);
    st.stopBalancer();
}

jsTest.log("Changed uuid causes defragmentation to restart");
{
    const coll = getNewColl();
    const nss = coll.getFullName();
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {key: 1}}));
    // Create two chunks on shard0
    coll.insertOne({key: -1, key2: -1});
    coll.insertOne({key: 1, key2: 1});
    assert.commandWorked(db.adminCommand({split: nss, middle: {key: 1}}));
    // Pause defragmentation after initialization but before phase 1 runs
    let configRSFailPoints = configureFailPointForRS(
        st.configRS.nodes, "afterBuildingNextDefragmentationPhase", {}, "alwaysOn");
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: nss,
        defragmentCollection: true,
        chunkSize: targetChunkSizeMB,
    }));
    st.startBalancer();
    // Reshard collection
    assert.commandWorked(db.adminCommand({reshardCollection: nss, key: {key2: 1}}));
    // Let defragementation run
    configRSFailPoints.off();
    defragmentationUtil.waitForEndOfDefragmentation(st.s, nss);
    st.stopBalancer();
    // Ensure the defragmentation succeeded
    const numChunksEnd = findChunksUtil.countChunksForNs(st.config, nss);
    assert.eq(numChunksEnd, 1);
}

jsTest.log("Refined shard key causes defragmentation to restart");
{
    const coll = getNewColl();
    const nss = coll.getFullName();
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {key: 1}}));
    // Create two chunks on shard0
    coll.insertOne({key: -1, key2: -1});
    coll.insertOne({key: 1, key2: 1});
    assert.commandWorked(db.adminCommand({split: nss, middle: {key: 1}}));
    // Pause defragmentation after initialization but before phase 1 runs
    let configRSFailPoints = configureFailPointForRS(
        st.configRS.nodes, "afterBuildingNextDefragmentationPhase", {}, "alwaysOn");
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: nss,
        defragmentCollection: true,
        chunkSize: targetChunkSizeMB,
    }));
    st.startBalancer();
    // Refine shard key - shouldn't change uuid
    assert.commandWorked(coll.createIndex({key: 1, key2: 1}));
    assert.commandWorked(db.adminCommand({refineCollectionShardKey: nss, key: {key: 1, key2: 1}}));
    // Let defragementation run
    configRSFailPoints.off();
    defragmentationUtil.waitForEndOfDefragmentation(st.s, nss);
    st.stopBalancer();
    // Ensure the defragmentation succeeded
    const numChunksEnd = findChunksUtil.countChunksForNs(st.config, nss);
    assert.eq(numChunksEnd, 1);
}

st.stop();
})();
