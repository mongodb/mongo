/**
 * Verifies that a change stream which is resumed on a downgraded binary does not crash
 * the server, even when reading oplog entries which the downgraded binary may not understand.
 *
 * @tags: [uses_change_streams, requires_replication]
 */
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster.

// Checking UUID consistency uses cached connections, which are not valid across restarts or
// stepdowns.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

const dbName = jsTestName();
const collName = "coll";

// Start a sharded cluster with latest binaries.
const st = new ShardingTest({
    shards: 1,
    rs: {
        nodes: 2,
        binVersion: "latest",
        setParameter: {logComponentVerbosity: '{command: {verbosity: 2}}'}
    },
    other: {mongosOptions: {binVersion: "latest"}}
});

let shardedColl = st.s.getDB(dbName)[collName];
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: shardedColl.getFullName(), key: {sk: 1}}));

//  Define a set of standard write tests. These tests will be run for every new version and should
//  not be modified. Each test case should have a function with field name 'generateOpLogEntry'
//  which takes a collection object as input.
const standardTestCases = [
    // Basic insert case.
    {
        testName: "StandardInsert",
        generateOpLogEntry: function(coll) {
            assert.commandWorked(coll.runCommand({
                insert: coll.getName(),
                documents: [{sk: 1}, {sk: 2}, {sk: -1}, {sk: -2}],
                ordered: false
            }));
        }
    },
    // Op-style update.
    {
        testName: "OpStyleUpdate",
        generateOpLogEntry: function(coll) {
            assert.commandWorked(coll.runCommand({
                update: shardedColl.getName(),
                updates: [
                    {q: {sk: 1}, u: {$set: {a: 1}}},
                    {q: {sk: -1}, u: {$set: {a: 1}}},
                    {q: {sk: 2}, u: {$set: {a: 1}}}
                ],
                ordered: false
            }));
        }
    },
    // Replacement style update.
    {
        testName: "ReplacementStyleUpdate",
        generateOpLogEntry: function(coll) {
            assert.commandWorked(coll.runCommand({
                update: shardedColl.getName(),
                updates: [{q: {sk: 1}, u: {sk: 1, a: 2}}, {q: {sk: -1}, u: {sk: -1, a: 2}}]
            }));
        }

    },
    // Pipeline style update.
    {
        testName: "PipelineStyleUpdate",
        generateOpLogEntry: function(coll) {
            assert.commandWorked(coll.runCommand({
                update: shardedColl.getName(),
                updates: [{q: {sk: 2}, u: [{$set: {a: 3}}]}, {q: {sk: -2}, u: [{$set: {a: 3}}]}]
            }));
        }
    },
    // Basic delete.
    {
        testName: "Delete",
        generateOpLogEntry: function(coll) {
            assert.commandWorked(coll.runCommand({
                delete: shardedColl.getName(),
                deletes: [{q: {sk: 1}, limit: 0}, {q: {sk: -2}, limit: 0}, {q: {sk: -1}, limit: 0}]
            }));
        }
    }
];

// The list of test cases against which to test the downgraded change stream. Any time a change is
// made to the existing oplog format, or whenever a new oplog entry type is created, a test-case
// should be added here which generates an example of the new or modified entry.
const latestVersionTestCases = [];

// Concatenate the standard tests with the custom latest-version tests to produce the final set of
// test-cases that will be run.
const testCases = standardTestCases.concat(latestVersionTestCases);

// The list of all the change stream variations against which the above test cases need to be run.
// Each entry should have function with field name 'watch' which opens a new change stream.
const changeStreamsVariants = [
    {
        watch: function(options) {
            return shardedColl.watch([], options);
        }
    },
    {
        watch: function(options) {
            return shardedColl.getDB().watch([], options);
        }
    },
    {
        watch: function(options) {
            return st.s.watch([], options);
        }
    },
    {
        watch: function(options) {
            return shardedColl.watch([], Object.assign(options, {fullDocument: "updateLookup"}));
        }
    },
    {
        watch: function(options) {
            return shardedColl.getDB().watch(
                [], Object.assign(options, {fullDocument: "updateLookup"}));
        }
    },
    {
        watch: function(options) {
            return st.s.watch([], Object.assign(options, {fullDocument: "updateLookup"}));
        }
    }
];

function dumpLatestOpLogEntries(node, limit) {
    const oplog = node.getCollection("local.oplog.rs");
    return oplog.find().sort({"ts": -1}).limit(limit).toArray();
}

/**
 * For each test case and change stream variation, generates the oplog entries to be tested and
 * creates an augmented test-case containing a resume token which marks the start of the test, and a
 * sentinel entry that marks the end of the test. These will be used post-downgrade to run each of
 * the test cases in isolation.
 */
function writeOplogEntriesAndCreateResumePointsOnLatestVersion() {
    function createSentinelEntryAndGetTimeStamp(testNum) {
        const documentId = "sentinel_entry_" + testNum;
        assert.commandWorked(shardedColl.insert({_id: documentId}));

        // Find the oplog entry for the document inserted above, and return its timestamp.
        const oplog = st.rs0.getPrimary().getCollection("local.oplog.rs");
        const opLogEntries =
            oplog.find({op: "i", "o._id": documentId, ns: shardedColl.getFullName()}).toArray();
        assert.eq(opLogEntries.length, 1);
        return opLogEntries[0].ts;
    }

    // We write a sentinel entry before each test case so that the resumed changestreams will have a
    // known point at which to stop while running each test.
    let testNum = 0;
    let testStartTime = createSentinelEntryAndGetTimeStamp(testNum);
    const outputChangeStreams = [];
    for (let testCase of testCases) {
        jsTestLog(`Opening a change stream for '${testCase.testName}' at startTime: ${
            tojson(testStartTime)}`);

        // Capture the 'resumeToken' when the sentinel entry is found. We use the token to resume
        // the stream rather than the 'testStartTime' because resuming from a token adds more stages
        // to the $changeStream pipeline, which increases our coverage.
        let resumeToken;
        const csCursor = changeStreamsVariants[0].watch({startAtOperationTime: testStartTime});
        assert.soon(
            () => {
                if (!csCursor.hasNext()) {
                    return false;
                }
                const nextEvent = csCursor.next();
                resumeToken = nextEvent._id;
                return (nextEvent.documentKey._id == "sentinel_entry_" + testNum);
            },
            () => {
                return tojson(dumpLatestOpLogEntries(st.rs0.getPrimary(), 100));
            });

        for (let changeStreamVariant of changeStreamsVariants) {
            // Start a change stream on the sentinel entry for each test case.
            const outputChangeStream = {
                watch: changeStreamVariant.watch,
                resumeToken: resumeToken,
                // The termination for the change stream of the current test case will be the
                // sentinel entry for the next test case.
                endSentinelEntry: "sentinel_entry_" + (testNum + 1),
                // We copy this for debugging purposes only.
                testName: testCases.testName
            };
            outputChangeStreams.push(outputChangeStream);
        }

        // Run the test case's 'generateOpLogEntry' function, which will create the actual oplog
        // entry to be tested.
        testCase.generateOpLogEntry(shardedColl);

        // Insert a sentinel to separate this test-case from the next.
        testStartTime = createSentinelEntryAndGetTimeStamp(++testNum);
    }
    return outputChangeStreams;
}

/**
 * Validates that resuming each of the change stream will not crash the server. The 'changeStreams'
 * should be an array and each entry should have fields 'watch', 'resumeToken' and
 * 'endSentinelEntry'.
 */
function resumeStreamsOnDowngradedVersion(changeStreams) {
    for (let changeStream of changeStreams) {
        jsTestLog("Validating change stream for " + tojson(changeStream));
        const csCursor = changeStream.watch({resumeAfter: changeStream.resumeToken});

        // Keep calling 'getmore' until the sentinal entry for the next test is found or until the
        // change stream throws an error.
        assert.soon(() => {
            if (!csCursor.hasNext()) {
                return false;
            }
            try {
                const nextEvent = csCursor.next();
                return (nextEvent.documentKey._id == changeStream.endSentinelEntry);
            } catch (e) {
                jsTestLog("Error occurred while reading change stream. " + tojson(e));

                // Validate that the error returned was not a consequence of server crash.
                assert.commandWorked(shardedColl.runCommand({ping: 1}));
                assert.commandWorked(st.rs0.getPrimary().getDB("admin").runCommand({ping: 1}));
                assert.commandWorked(st.configRS.getPrimary().getDB("admin").runCommand({ping: 1}));

                return true;
            }
        });
    }
}

// Obtain the list of change stream tests and the associated tokens from which to resume after the
// cluster has been downgraded.
const changeStreamsToBeValidated = writeOplogEntriesAndCreateResumePointsOnLatestVersion();

function runTests(downgradeVersion) {
    jsTestLog("Running test with 'downgradeVersion': " + downgradeVersion);
    const downgradeFCV = downgradeVersion === "last-lts" ? lastLTSFCV : lastContinuousFCV;
    // Downgrade the entire cluster to the 'downgradeVersion' binVersion.
    assert.commandWorked(
        st.s.getDB(dbName).adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    st.upgradeCluster(downgradeVersion);

    // Refresh our reference to the sharded collection post-downgrade.
    shardedColl = st.s.getDB(dbName)[collName];

    // Resume all the change streams that were created on latest version and validate that the
    // change stream doesn't crash the server after downgrade.
    resumeStreamsOnDowngradedVersion(changeStreamsToBeValidated);
}

// Test resuming change streams after downgrading the cluster to 'last-continuous'.
runTests('last-continuous');

// Upgrade the entire cluster back to the latest version.
st.upgradeCluster('latest', {waitUntilStable: true});
assert.commandWorked(st.s.getDB(dbName).adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Test resuming change streams after downgrading the cluster to 'last-lts'.
runTests('last-lts');
st.stop();
}());
