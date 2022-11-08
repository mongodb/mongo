/**
 * Tests simple exclusion projections. Note that there is overlap in coverage with
 * jstests/cqf/projection.js; both tests will exist pending a decision about the future of golden
 * jstesting for CQF.
 */

(function() {
"use strict";
load("jstests/query_golden/libs/projection_helpers.js");

const coll = db.cqf_exclusion_project;
const exclusionProjSpecs = [
    {a: 0},
    {a: 0, _id: 0},
    {a: 0, x: 0},

    {"a.b": 0},
    {"a.b": 0, "a.c": 0},

    {"a.b.c": 0},
    {"a.b.c": 0, "a.b.d": 0},

    // This syntax is permitted and equivalent to the dotted notation.
    {a: {b: 0}},
    // A mix of dotted syntax and nested syntax is permitted as well.
    {a: {"b.c": {d: 0, e: 0}}},
];
runProjectionsAgainstColl(coll, getProjectionDocs(), [] /*no indexes*/, exclusionProjSpecs);

const idExclusionProjectSpecs = [
    {_id: 0},
    {"_id.a": 0},
    {"_id.a": 0, "_id.b": 0},
    {"_id.a.b": 0},
];
runProjectionsAgainstColl(coll, getIdProjectionDocs(), [] /*no indexes*/, idExclusionProjectSpecs);
}());
