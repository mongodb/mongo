/**
 * Test that $densify fails with the right uassert codes when collections have values with the wrong
 * types, and mixed value types in the same collection.
 * @tags: [
 *   # Needed as $densify is a 51 feature.
 *   requires_fcv_51,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.

const coll = db[jsTestName()];
coll.drop();

// Densification on a numeric collection with a date step.
assert.commandWorked(coll.insert([{val: 1}, {val: 3}, {val: 5}]));
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$densify: {field: "val", range: {step: 1, bounds: "full", unit: "day"}}}],
    cursor: {}
}),
                             6053600);

coll.drop();

// Densification on a date collection with a numeric step.
assert.commandWorked(coll.insert([
    {val: new ISODate("2021-01-01")},
    {val: new ISODate("2021-01-03")},
    {val: new ISODate("2021-01-05")}
]));
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$densify: {field: "val", range: {step: 1, bounds: "full"}}}],
    cursor: {}
}),
                             6053600);
coll.drop();

// Densification on a mixed collection with a numeric step and a first value that's numeric.
assert.commandWorked(coll.insert([
    {val: 1},
    {val: new ISODate("2021-01-01")},
    {val: new ISODate("2021-01-03")},
    {val: new ISODate("2021-01-05")}
]));
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$densify: {field: "val", range: {step: 1, bounds: "full"}}}],
    cursor: {}
}),
                             6053600);
coll.drop();

// Densification on a mixed collection with a date step and a first value that's numeric.
assert.commandWorked(coll.insert([
    {val: 1},
    {val: new ISODate("2021-01-01")},
    {val: new ISODate("2021-01-03")},
    {val: new ISODate("2021-01-05")}
]));
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$densify: {field: "val", range: {step: 1, bounds: "full", unit: "day"}}}],
    cursor: {}
}),
                             6053600);
coll.drop();

// Densification on a mixed collection with a numeric step and a first value that's a date.
assert.commandWorked(coll.insert([
    {val: new ISODate("2021-01-01")},
    {val: new ISODate("2021-01-03")},
    {val: 1},
    {val: new ISODate("2021-01-05")}
]));
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$densify: {field: "val", range: {step: 1, bounds: "full"}}}],
    cursor: {}
}),
                             6053600);

coll.drop();
// Densification on a mixed collection with a date step and a first value that's a date.
assert.commandWorked(coll.insert([
    {val: new ISODate("2021-01-01")},
    {val: new ISODate("2021-01-03")},
    {val: 1},
    {val: new ISODate("2021-01-05")}
]));
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$densify: {field: "val", range: {step: 1, bounds: "full", unit: "day"}}}],
    cursor: {}
}),
                             6053600);

coll.drop();
})();
