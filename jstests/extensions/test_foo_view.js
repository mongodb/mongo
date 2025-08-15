/**
 * This test checks that $testFoo works in a view definition, on a view, and both in and on a view.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insertMany([{x: 1}, {x: 2}, {x: 3}]));

// View -> [{x: 1, y: 10}, {x: 2, y: 10}, {x: 3, y: 10}]
const baseViewPipeline = [{$addFields: {y: 10}}, {$project: {_id: 0}}];
const baseUserPipeline = [{$match: {x: {$gte: 2}}}];
const expectedResults = [{x: 2, y: 10}, {x: 3, y: 10}];

const fooViewName = "foo_view_on_" + jsTestName();
const regularViewName = "regular_view_on_" + jsTestName();

// $testFoo in a view definition.
assert.commandWorked(
    db.createView(fooViewName, jsTestName(), [{$testFoo: {}}, ...baseViewPipeline]));
let res = db[fooViewName].aggregate(baseUserPipeline).toArray();
assertArrayEq({actual: res, expected: expectedResults});

// $testFoo on a view.
assert.commandWorked(db.createView(regularViewName, jsTestName(), baseViewPipeline));
res = db[regularViewName].aggregate([{$testFoo: {}}, ...baseUserPipeline]).toArray();
assertArrayEq({actual: res, expected: expectedResults});

// $testFoo in and on a view.
res = db[fooViewName].aggregate([{$testFoo: {}}, ...baseUserPipeline]).toArray();
assertArrayEq({actual: res, expected: expectedResults});

// $testFoo in a nested view.
const nestedViewName = "nested_foo_view_on_" + jsTestName();
const nestedViewPipeline = [{$testFoo: {}}, {$addFields: {z: 20}}];
assert.commandWorked(db.createView(nestedViewName, fooViewName, nestedViewPipeline));
res = db[nestedViewName].aggregate([{$testFoo: {}}, ...baseUserPipeline]).toArray();
assertArrayEq({actual: res, expected: [{x: 2, y: 10, z: 20}, {x: 3, y: 10, z: 20}]});
