/**
 * Verify that $sample push down works when setting 'internalQueryCutoffForSampleFromRandomCursor'
 * cluster paramater.
 *
 * Requires random cursor support.
 * @tags: [requires_replication]
 */
(function() {
'use strict';

load('jstests/libs/analyze_plan.js');  // For planHasStage.

const numDocs = 1000;
const sampleSize = numDocs * .06;
let docs = [];
for (let i = 0; i < numDocs; ++i) {
    docs.push({a: i});
}

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const collName = 'sample_pushdown';
const dbName = 'admin';
const testDB = rst.getPrimary().getDB(dbName);
const coll = testDB[collName];
assert.commandWorked(coll.insert(docs));
const pipeline = [{$sample: {size: sampleSize}}, {$match: {a: {$gte: 0}}}];

// Tests that with the default value for the cluster parameter (5%), the constructed plan will not
// use a storage engine random cursor since our sample size is more than 5% of the number of
// documents in our collection.
(function testDefaultClusterParamValue() {
    // // Verify that our pipeline uses $sample push down.
    const explain = coll.explain().aggregate(pipeline);
    assert(!aggPlanHasStage(explain, "$sampleFromRandomCursor"), tojson(explain));

    // Run the pipeline.
    const randDocs = testDB[collName].aggregate(pipeline).toArray();

    // Verify that we have the correct number of docs.
    assert.eq(randDocs.length, sampleSize, tojson(randDocs));
})();

// Tests that with the setting the cluster parameter value to 7%, the constructed plan will use a
// storage engine random cursor since our sample size is less than 7% of the number of documents in
// our collection.
(function testNotDefaultClusterParamValue() {
    // Try to set the cluster parameter to 0, should fail since the value must be gt 0 and lte 1.
    const clusterParameterValue0 = {sampleCutoff: 0};
    const clusterParameterName0 = 'internalQueryCutoffForSampleFromRandomCursor';
    const clusterParameter0 = {[clusterParameterName0]: clusterParameterValue0};
    assert.commandFailedWithCode(testDB.runCommand({setClusterParameter: clusterParameter0}),
                                 51024);

    // Set the cluster parameter to have a cutoff of 7%.
    const clusterParameterValue = {sampleCutoff: 0.07};
    const clusterParameterName = 'internalQueryCutoffForSampleFromRandomCursor';
    const clusterParameter = {[clusterParameterName]: clusterParameterValue};
    assert.commandWorked(testDB.runCommand({setClusterParameter: clusterParameter}));

    // Make sure this cluster parameter holds the correct value.
    const getClusterVal =
        assert
            .commandWorked(testDB.runCommand(
                {getClusterParameter: clusterParameterName}))["clusterParameters"][0]
            .sampleCutoff;
    assert.eq(getClusterVal, 0.07);

    // Verify that our pipeline uses $sample push down, since the sample size is less than 7% of the
    // number of documents in our collection.
    const explain = coll.explain().aggregate(pipeline);
    assert(aggPlanHasStage(explain, "$sampleFromRandomCursor"), tojson(explain));

    // Set the cluster parameter to have a cutoff of 1%.
    const clusterParameterValue1 = {sampleCutoff: 0.01};
    const clusterParameterName1 = 'internalQueryCutoffForSampleFromRandomCursor';
    const clusterParameter1 = {[clusterParameterName1]: clusterParameterValue1};
    assert.commandWorked(testDB.runCommand({setClusterParameter: clusterParameter1}));

    // Make sure this cluster parameter holds the correct value.
    const getClusterVal1 =
        assert
            .commandWorked(testDB.runCommand(
                {getClusterParameter: clusterParameterName1}))["clusterParameters"][0]
            .sampleCutoff;
    assert.eq(getClusterVal1, 0.01);

    // Verify that our pipeline does not use $sample push down, since the sample size is more than
    // 1% of the number of documents in our collection.
    const explain2 = coll.explain().aggregate(pipeline);
    assert(!aggPlanHasStage(explain2, "$sampleFromRandomCursor"), tojson(explain2));

    // Run the pipeline.
    const randDocs = testDB[collName].aggregate(pipeline).toArray();

    // Verify that we have the correct number of docs.
    assert.eq(randDocs.length, sampleSize, tojson(randDocs));
})();

// // Clean up.
rst.stopSet();
})();
