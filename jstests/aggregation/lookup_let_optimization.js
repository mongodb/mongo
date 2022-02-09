/**
 * This test ensures that stages dependent on a let variable optimizing to a constant in a $lookup
 * pipeline are evaluated correctly.
 * @tags: [
 *     requires_pipeline_optimization,
 *     assumes_unsharded_collection
 * ]
 */

load('jstests/aggregation/extras/utils.js');  // For assertArrayEq.
load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.

(function() {
"use strict";

const collName = "lookup_let_redact";
const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insert([
    {_id: "true", test: true},
    {_id: "false", test: false},
]));

const admin = db.getSiblingDB("admin");

const setPipelineOptimizationMode = (mode) => {
    FixtureHelpers.runCommandOnEachPrimary(
        {db: admin, cmdObj: {configureFailPoint: 'disablePipelineOptimization', mode}});
};

const verifyAggregationForBothPipelineOptimizationModes = ({pipeline, expected}) => {
    // Verify results when pipeline optimization is disabled.
    setPipelineOptimizationMode('alwaysOn');
    assertArrayEq({actual: coll.aggregate(pipeline).toArray(), expected});

    // Verify results when pipeline optimization is enabled.
    setPipelineOptimizationMode('off');
    assertArrayEq({actual: coll.aggregate(pipeline).toArray(), expected});
};

// Get initial optimization mode.
const pipelineOptParameter = assert.commandWorked(
    db.adminCommand({getParameter: 1, "failpoint.disablePipelineOptimization": 1}));
const oldMode =
    pipelineOptParameter["failpoint.disablePipelineOptimization"].mode ? 'alwaysOn' : 'off';

// Verify $redact.
verifyAggregationForBothPipelineOptimizationModes({
    pipeline: [
        {$lookup: {
            from: collName,
            let: {iShouldPrune: "$test"},
            pipeline: [
                {$redact: {$cond: {if: "$$iShouldPrune", then: "$$PRUNE", else: "$$DESCEND"}}}
            ],
            as: "redacted"
        }}
    ],
    expected: [
        {_id: "true", test: true, redacted: []}, // Expect that documents were pruned.
        {_id: "false", test: false, redacted: [ // Expect that $redact descended instead.
            {_id: "true", test: true},
            {_id: "false", test: false} 
        ]}
    ]
});

// Verify $unionWith.
verifyAggregationForBothPipelineOptimizationModes({
    pipeline: [
        {$lookup: {
            from: collName,
            let: {iShouldMatch: "$test"},
            pipeline: [
                {$unionWith: {
                    coll: collName,
                    pipeline: [{$match: {$expr: {$eq: ["$$iShouldMatch", "$test"]}}}]
                }}
            ],
            as: "united"
        }}
    ],
    expected: [
        // $unionWith should match and include only the document with {test: true} in 'united'.
        {_id: "true", test: true, united: [
            {_id: "true", test: true},
            {_id: "true", test: true},
            {_id: "false", test: false},
        ]},
        // $unionWith should match and include only the document with {test: false} in 'united'.
        {_id: "false", test: false, united: [
            {_id: "false", test: false},
            {_id: "false", test: false},
            {_id: "true", test: true},
        ]}
    ]
});

// Reset optimization mode.
setPipelineOptimizationMode(oldMode);
}());
