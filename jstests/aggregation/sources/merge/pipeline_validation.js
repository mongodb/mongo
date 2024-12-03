/**
 * Tests for the validatioon of 'whenMatched' field when it is a pipeline.
 * TODO SERVER-96515 remove this tag.
 * @tags: [known_query_shape_computation_problem]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const source = db[`${jsTestName()}_source`];
const target = db[`${jsTestName()}_target`];

[source, target].forEach(coll => coll.drop());

function mergeWith(pipeline) {
    return {$merge: {into: target.getName(), whenMatched: pipeline, whenNotMatched: "insert"}};
}
function assertPipelineIsInvalid(pipeline, expectedErrorCode) {
    assertErrorCode(source, mergeWith(pipeline), expectedErrorCode);
}

// Sharded clusters will hit this error earlier since they invoke the 'serialize()'
// codepath.
// TODO SERVER-97846 Investigate why this query only _sometimes_ errors when the collection is not
// sharded.
// TODO SERVER-96515 remove the branching here.
if (!FixtureHelpers.isMongos(db)) {
    assert.doesNotThrow(() => source.aggregate(mergeWith([{$addFields: 2}])));
}
assert.commandWorked(source.insert({_id: 0}));
assertPipelineIsInvalid([{$addFields: 2}], 40272);
