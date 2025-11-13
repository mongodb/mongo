/**
 * Tests for queries where a top-level field name is renamed to a dotted path. As of this writing
 * the system does not currently support match pushdown past these renames. This regression test
 * will ensure that future improvements to our match pushdown optimization does not break related
 * queries, especially those involving equality-to-object predicates.
 */

import {show} from "jstests/libs/golden_test.js";

const coll = db.field_renamed_to_dotted_path;
coll.drop();

const docs = [
    {_id: 1},
    {_id: 2, a: null},
    {_id: 3, a: 5},
    {_id: 4, a: {b: 1, c: 1}},
    {_id: 5, a: {b: 1, c: 1}},
    {_id: 6, a: {c: 1, b: 1}},
];

jsTestLog("Inserting docs:");
show(docs);
coll.insert(docs);

function testPipeline(pipeline) {
    print("Pipeline: " + tojsononeline(pipeline));
    show(coll.aggregate(pipeline));
    print("\n");
}

jsTestLog("Running test pipelines:");

testPipeline([{$addFields: {"x.y": "$a"}}, {$match: {x: {$eq: {y: 5}}}}]);
testPipeline([{$addFields: {"x.y": "$a"}}, {$match: {x: {$eq: {y: {b: 1, c: 1}}}}}]);
testPipeline([{$addFields: {"x.y": "$a"}}, {$match: {"x.y": {$eq: {b: 1, c: 1}}}}]);

// Repeat the tests above, except use $expr.
testPipeline([{$addFields: {"x.y": "$a"}}, {$match: {$expr: {$eq: ["$x", {y: 5}]}}}]);
testPipeline([{$addFields: {"x.y": "$a"}}, {$match: {$expr: {$eq: ["$x", {y: {b: 1, c: 1}}]}}}]);
testPipeline([{$addFields: {"x.y": "$a"}}, {$match: {$expr: {$eq: ["$x.y", {b: 1, c: 1}]}}}]);
