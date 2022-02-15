/**
 * This test ensures that stages dependent on a let variable optimizing to a constant in a $lookup
 * pipeline are evaluated correctly.
 * @tags: [assumes_unsharded_collection]
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

const verifyAggregationResults = ({pipeline, expected}) => {
    assertArrayEq({actual: coll.aggregate(pipeline).toArray(), expected});
};

// Verify $redact.
verifyAggregationResults({
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
}());
