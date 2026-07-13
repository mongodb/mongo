/**
 * Test that samplingCE works properly in a replica set environment. This is a regression test for
 * SERVER-107657.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {getAllPlans} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let docs = [];
for (let i = 0; i < 1000; i++) {
    docs.push({a: i, b: i % 10});
}

const samplingConfigs = [
    {
        setParameter: {
            featureFlagCostBasedRanker: true,
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "samplingCE",
            // TODO SERVER-117707: remove once CBR supports SBE.
            internalQueryFrameworkControl: "forceClassicEngine",
        },
    },
    {
        setParameter: {
            featureFlagCostBasedRanker: true,
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "samplingCE",
            internalQuerySamplingCEMethod: "chunk",
            // TODO SERVER-117707: remove once CBR supports SBE.
            internalQueryFrameworkControl: "forceClassicEngine",
        },
    },
    {
        setParameter: {
            featureFlagCostBasedRanker: true,
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "samplingCE",
            internalQuerySamplingBySequentialScan: true,
            // TODO SERVER-117707: remove once CBR supports SBE.
            internalQueryFrameworkControl: "forceClassicEngine",
        },
    },
];

for (const config of samplingConfigs) {
    jsTest.log.info(`Testing with config: ${tojson(config)}`);
    const rst = new ReplSetTest({nodes: 2, nodeOptions: config});

    rst.startSet();
    rst.initiate();

    const db = rst.getPrimary().getDB("test");
    // TODO SERVER-92589: Remove this exemption once CBR supports SBE.
    if (checkSbeRestrictedOrFullyEnabled(db)) {
        jsTest.log.info(`Skipping test because CBR does not support SBE`);
        rst.stopSet();
        quit();
    }

    // Create a collection and insert some data.
    const coll = db[jsTestName()];
    coll.drop();
    assert.commandWorked(coll.insert(docs));
    assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

    // Run a query with explain and check that the plans are costed using samplingCE. On a replica
    // set the sampling read relies on majority-committed data, which can briefly lag the inserts
    // above. Poll until every candidate plan is costed via sampling.
    const samplingQuery = {a: {$gt: 500}, b: {$lt: 5}};
    assert.soon(
        () => {
            const plans = getAllPlans(coll.find(samplingQuery).explain());
            return (
                plans.length > 0 &&
                plans.every(
                    (plan) =>
                        plan.estimatesMetadata?.ceSource === "Sampling" &&
                        plan.hasOwnProperty("cardinalityEstimate"),
                )
            );
        },
        () =>
            "Expected all plans to be costed via Sampling; last explain: " +
            tojson(coll.find(samplingQuery).explain()),
    );

    rst.stopSet();
}
