/**
 * Tests that $merge with {whenMatched: [], whenNotMatched: 'insert'} is handled correctly during
 * upgrade from and downgrade to a pre-backport version of 4.2 on a sharded cluster.
 */
(function() {
"use strict";

load("jstests/multiVersion/libs/causal_consistency_helpers.js");  // supportsMajorityReadConcern
load("jstests/multiVersion/libs/multi_cluster.js");               // upgradeCluster
load("jstests/multiVersion/libs/multi_rs.js");                    // upgradeSet

// The UUID consistency check can hit NotPrimaryNoSecondaryOk when it attempts to obtain a list of
// collections from the shard Primaries through mongoS at the end of this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

if (!supportsMajorityReadConcern()) {
    jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
    return;
}

const preBackport42Version = "4.2.1";
const latestVersion = "latest";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {nodes: 3},
    other: {
        mongosOptions: {binVersion: preBackport42Version},
        configOptions: {binVersion: preBackport42Version},
        rsOptions: {binVersion: preBackport42Version},
    }
});

// Obtain references to the test database, the source and target collections.
let mongosDB = st.s.getDB(jsTestName());
let sourceSharded = mongosDB.source_coll_sharded;
let targetSharded = mongosDB.target_coll_sharded;
let sourceUnsharded = mongosDB.source_coll_unsharded;
let targetUnsharded = mongosDB.target_coll_unsharded;

// Updates the specified cluster components and then refreshes our references to each of them.
function refreshCluster(version, components, singleShard) {
    // Default to only upgrading the explicitly specified components.
    const defaultComponents = {upgradeMongos: false, upgradeShards: false, upgradeConfigs: false};
    components = Object.assign(defaultComponents, components);

    if (singleShard) {
        singleShard.upgradeSet({binVersion: version});
        st.waitUntilStable();
    } else {
        const upgradeOptions = Object.assign({waitUntilStable: true}, components);
        st.upgradeCluster(version, upgradeOptions);
    }

    // Having upgraded the cluster, reacquire references to each component.
    mongosDB = st.s.getDB(jsTestName());
    sourceSharded = mongosDB.source_coll_sharded;
    targetSharded = mongosDB.target_coll_sharded;
    sourceUnsharded = mongosDB.source_coll_unsharded;
    targetUnsharded = mongosDB.target_coll_unsharded;
}

// Run the aggregation and swallow applicable exceptions for as long as we receive them, up to the
// assert.soon timeout. This is necessary because there is a period after one shard's Primary steps
// down during upgrade where a $merge on the other shard may still target the previous Primary.
function tryWhileNotPrimary(sourceColl, targetColl, pipeline, options) {
    assert.soonNoExcept(() => {
        const aggCmdParams = Object.assign({pipeline: pipeline, cursor: {}}, options);
        const cmdRes = sourceColl.runCommand("aggregate", aggCmdParams);
        if (cmdRes.ok) {
            return true;
        }
        // The only errors we are prepared to swallow are ErrorCodes.NotWritablePrimary and
        // CursorNotFound. The latter can be thrown as a consequence of a NotWritablePrimary on one
        // shard when the $merge stage is dispatched to a merging shard as part of the latter half
        // of the pipeline.
        const errorsToSwallow = [ErrorCodes.NotWritablePrimary, ErrorCodes.CursorNotFound];
        assert(errorsToSwallow.includes(cmdRes.code), () => tojson(cmdRes));
        // TODO SERVER-43851: this may be susceptible to zombie writes. Ditto for all other
        // occurrences of remove({}) throughout this test.
        assert.commandWorked(targetColl.remove({}));
        return false;
    });
}

// Enable sharding on the the test database and ensure that the primary is shard0.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

// Shard the source collection on {_id: 1}, split across the shards at {_id: 0}.
st.shardColl(sourceSharded, {_id: 1}, {_id: 0}, {_id: 1});

// Shard the target collection on {_id: "hashed"}, so that the target shard for each document will
// not necessarily be the same as the source shard.
st.shardColl(targetSharded, {_id: "hashed"}, false, false);

// Insert an identical set of test data into both the sharded and unsharded source collections. In
// the former case, the documents are spread across both shards.
for (let i = -20; i < 20; ++i) {
    assert.commandWorked(sourceSharded.insert({_id: i}));
    assert.commandWorked(sourceUnsharded.insert({_id: i}));
}

// Define a series of test cases covering all $merge distributed planning scenarios.
const testCases = [];
for (let sourceColl of [sourceSharded, sourceUnsharded]) {
    for (let targetColl of [targetSharded, targetUnsharded]) {
        for (let preMergePipeline of [[], [{$group: {_id: "$_id"}}]]) {
            for (let disableExchange of [false, true]) {
                for (let allowDiskUse of [false, true]) {
                    testCases.push({
                        sourceColl: () => sourceColl,
                        targetColl: () => targetColl,
                        preMergePipeline: preMergePipeline,
                        allowDiskUse: allowDiskUse,
                        disableExchange: disableExchange
                    });
                }
            }
        }
    }
}

// The 'whenMatched' pipeline to apply as part of the $merge. When the old 4.2.1 behaviour is in
// effect, output documents will all have an _id field and the field added by this pipeline.
const mergePipe = [{$addFields: {docWasGeneratedFromWhenMatchedPipeline: true}}];

// Generate the array of output documents we expect to see under the old upsert behaviour.
const expectedOldBehaviourOutput = Array.from(sourceSharded.find().toArray(), (doc) => {
    return {_id: doc._id, docWasGeneratedFromWhenMatchedPipeline: true};
});

// Generate the array of output documents we expect to see under the new upsert behaviour.
const expectedNewBehaviourOutput = sourceSharded.find().toArray();

// Run all test cases, validating that the specified output is produced in each case.
function runAllTestCases(expectedOutput) {
    for (let testCase of testCases) {
        // Print out the current testcase in the logs.
        jsTestLog(tojson(Object.assign({}, testCase, {
            sourceColl: testCase.sourceColl().getName(),
            targetColl: testCase.targetColl().getName()
        })));

        // Perform initial test-case setup. Disable the exchange optimization if appropriate.
        assert.commandWorked(mongosDB.adminCommand(
            {setParameter: 1, internalQueryDisableExchange: testCase.disableExchange}));

        // Construct the options object that will be supplied along with the pipeline.
        const aggOptions = {allowDiskUse: testCase.allowDiskUse};

        // Construct the final pipeline by appending $merge to the the testCase's preMergePipeline.
        const finalPipeline = testCase.preMergePipeline.concat([{
            $merge: {
                into: testCase.targetColl().getName(),
                whenMatched: mergePipe,
                whenNotMatched: "insert"
            }
        }]);

        // If exchange is disabled for this test, make sure that we don't see a plan that uses it.
        if (testCase.disableExchange) {
            const explainOut = testCase.sourceColl().explain().aggregate(finalPipeline, aggOptions);
            assert.neq(explainOut.mergeType, "exchange");
        }

        // Run the test case and confirm that the target collection matches the expected output.
        tryWhileNotPrimary(testCase.sourceColl(), testCase.targetColl(), finalPipeline, aggOptions);
        assert.sameMembers(testCase.targetColl().find().toArray(), expectedOutput);
        assert.commandWorked(testCase.targetColl().remove({}));
    }
}

// Run all $merge test-cases with the whole cluster on 'preBackport42Version' and confirm that the
// output documents are produced using the old upsert behaviour.
jsTestLog(`UPGRADE: Running tests with whole cluster on ${preBackport42Version}`);
runAllTestCases(expectedOldBehaviourOutput);

// Upgrade a single shard to latest but leave the mongoS on 'preBackport42Version'. The upgraded
// shard continues to produce upsert requests that are compatible with the pre-backport shards.
jsTestLog(`UPGRADE: rs1 to ${latestVersion}; rs0, configs & mongos on ${preBackport42Version}`);
refreshCluster(latestVersion, null, st.rs1);
runAllTestCases(expectedOldBehaviourOutput);

// Upgrade the configs and the remaining shard to latest but leave mongoS on pre-backport 4.2. The
// shards continue to produce upsert requests that use the pre-backport behaviour. This is to ensure
// that the pipeline produces the same behaviour regardless of whether $merge is pushed down to the
// shards or run on the mongoS itself.
jsTestLog(`UPGRADE: shards and configs to ${latestVersion}; mongos on ${preBackport42Version}`);
refreshCluster(latestVersion, {upgradeShards: true, upgradeConfigs: true});
runAllTestCases(expectedOldBehaviourOutput);

// Upgrade the mongoS to latest. We should now see that the $merge adopts the new behaviour, and
// inserts the exact source document rather than generating one from the whenMatched pipeline.
jsTestLog(`UPGRADE: mongos to ${latestVersion}; whole cluster now on ${latestVersion}`);
refreshCluster(latestVersion, {upgradeMongos: true});
runAllTestCases(expectedNewBehaviourOutput);

st.stop();
})();
