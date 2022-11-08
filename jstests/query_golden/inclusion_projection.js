/**
 * Tests simple inclusion projections. Note that there is overlap in coverage with
 * jstests/cqf/projection.js; both tests will exist pending a decision about the future of golden
 * jstesting for CQF.
 */

(function() {
"use strict";
load("jstests/query_golden/libs/projection_helpers.js");

const coll = db.cqf_inclusion_project;

const inclusionProjSpecs = [
    {a: 1},
    {a: 1, _id: 1},
    {a: 1, _id: 0},
    {a: 1, x: 1},

    {"a.b": 1},
    {"a.b": 1, _id: 0},
    {"a.b": 1, "a.c": 1},

    {"a.b.c": 1},
    {"a.b.c": 1, _id: 0},
    {"a.b.c": 1, "a.b.d": 1},

    // This syntax is permitted and equivalent to the dotted notation.
    {a: {b: 1}},
    // A mix of dotted syntax and nested syntax is permitted as well.
    {a: {"b.c": {d: 1, e: 1}}},
];
const indexes = [{"a": 1}, {"a": -1}, {"a.b": 1}, {"a.b": 1, "a.c": 1}, {"a.b.c": 1}];
runProjectionsAgainstColl(coll, getProjectionDocs(), indexes, inclusionProjSpecs);

// Show that inclusion of subpaths of _id is not special; it works the same way.
// Test without indexes this time.
const idInclusionProjSpecs = [
    {_id: 1},
    {"_id.a": 1},
    {"_id.a": 1, "_id.b": 1},
    {"_id.a.b": 1},
];
const idIndexes = [{"_id.a": 1}, {"_id.a": 1, "_id.b": 1}, {"_id.a.b": 1}];
runProjectionsAgainstColl(coll, getIdProjectionDocs(), idIndexes, idInclusionProjSpecs);
}());
