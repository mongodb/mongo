/*
 * Test that samplingCE works properly in a replica set environment. This is a regression test for
 * SERVER-107657.
 */

import {getAllPlans} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let docs = [];
for (let i = 0; i < 1000; i++) {
    docs.push({a: i, b: i % 10});
}

const samplingConfigs = [
    {setParameter: {planRankerMode: "samplingCE"}},
    {setParameter: {planRankerMode: "samplingCE", internalQuerySamplingCEMethod: "chunk"}},
    {setParameter: {planRankerMode: "samplingCE", internalQuerySamplingBySequentialScan: true}},
];

for (const config of samplingConfigs) {
    jsTestLog(`Testing with config: ${tojson(config)}`);
    const rst = new ReplSetTest({nodes: 3, nodeOptions: config});

    rst.startSet();
    rst.initiate();

    const db = rst.getPrimary().getDB("test");
    // TODO SERVER-92589: Remove this exemption
    if (checkSbeFullyEnabled(db)) {
        jsTestLog(`Skipping test because CBR does not support SBE`);
        rst.stopSet();
        quit();
    }

    // Create a collection and insert some data.
    const coll = db[jsTestName()];
    coll.drop();
    assert.commandWorked(coll.insert(docs));
    assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

    // Run a query with explain and check that the plans are costed using samplingCE.
    const explain = coll.find({a: {$gt: 500}, b: {$lt: 5}}).explain();
    getAllPlans(explain).forEach((plan) => {
        assert.eq(plan.estimatesMetadata.ceSource, "Sampling", plan);
        assert(plan.hasOwnProperty("cardinalityEstimate"));
    });

    rst.stopSet();
}
