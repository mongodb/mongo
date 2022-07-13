/**
 * Test that $linearFill works as a window function.
 * @tags: [
 *   requires_fcv_52,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");
load("jstests/aggregation/extras/utils.js");  // For arrayEq.
load("jstests/libs/feature_flag_util.js");    // For isEnabled.

const coll = db.linear_fill;
coll.drop();

// Test that $linearFill doesn't parse with a window.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            sortBy: {_id: 1},
            output: {val: {$linearFill: {}, window: []}},
        }
    }],
    cursor: {}
}),
                             ErrorCodes.FailedToParse);

let collection = [
    {_id: 0, val: 0},
    {_id: 1, val: null},
    {_id: 2, val: null},
    {_id: 9, val: null},
    {_id: 10, val: 10},
];
assert.commandWorked(coll.insert(collection));

let result = coll.aggregate([{
                     $setWindowFields: {
                         sortBy: {_id: 1},
                         output: {val: {$linearFill: "$val"}},
                     }
                 }])
                 .toArray();

let expected = [
    {_id: 0, val: 0},
    {_id: 1, val: 1},
    {_id: 2, val: 2},
    {_id: 9, val: 9},
    {_id: 10, val: 10},
];
assert.eq(result, expected);

coll.drop();
collection = [
    {_id: 0, val: 0},
    {_id: 1, val: null},
    {_id: 3, val: 3},
    {_id: 9, val: 9},
    {_id: 10, val: null},
    {_id: 11, val: 11}
];
assert.commandWorked(coll.insert(collection));
result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {_id: 1},
                     output: {val: {$linearFill: "$val"}},
                 }
             }])
             .toArray();

expected = [
    {_id: 0, val: 0},
    {_id: 1, val: 1},
    {_id: 3, val: 3},
    {_id: 9, val: 9},
    {_id: 10, val: 10},
    {_id: 11, val: 11}
];
assert.eq(result, expected);

coll.drop();
collection = [
    {_id: 10, val: null},
    {_id: 0, val: 0},
    {_id: 11, val: 11},
    {_id: 1, val: null},
    {_id: 9, val: 9},
    {_id: 3, val: 3}
];
assert.commandWorked(coll.insert(collection));
result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {_id: 1},
                     output: {val: {$linearFill: "$val"}},
                 }
             }])
             .toArray();
assert.eq(result, expected);

coll.drop();
collection = [
    {_id: 0.5, val: 0},
    {_id: 1, val: null},
    {_id: 4.384, val: null},
    {_id: 9, val: null},
    {_id: 10.09283, val: 11.28374},
];
assert.commandWorked(coll.insert(collection));
result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {_id: 1},
                     output: {val: {$linearFill: "$val"}},
                 }
             }])
             .toArray();
expected = [
    {_id: 0.5, val: 0},
    {_id: 1, val: 0.5881340542884634},
    {_id: 4.384, val: 4.5686253337127845},
    {_id: 9, val: 9.998278922903879},
    {_id: 10.09283, val: 11.28374},
];

assert.eq(result, expected);

coll.drop();
collection = [
    {_id: 1, val: 1},
    {_id: 2, val: null},
    {_id: 3, val: null},
    {_id: 4, val: null},
    {_id: 5, val: 5},
    {_id: 6, val: null},
    {_id: 7, val: null},
    {_id: 8, val: null},
    {_id: 9, val: 10},
    {_id: 10, val: null},
    {_id: 11, val: null},
    {_id: 12, val: null},
    {_id: 13, val: -5},
];

expected = [
    {_id: 1, val: 1},
    {_id: 2, val: 2},
    {_id: 3, val: 3},
    {_id: 4, val: 4},
    {_id: 5, val: 5},
    {_id: 6, val: 6.25},
    {_id: 7, val: 7.5},
    {_id: 8, val: 8.75},
    {_id: 9, val: 10},
    {_id: 10, val: 6.25},
    {_id: 11, val: 2.5},
    {_id: 12, val: -1.25},
    {_id: 13, val: -5},
];
assert.commandWorked(coll.insert(collection));
result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {_id: 1},
                     output: {val: {$linearFill: "$val"}},
                 }
             }])
             .toArray();
assert.eq(result, expected);
coll.drop();

// Test dates.
collection = [
    {_id: 0, date: new Date(Date.UTC(2003, 11, 28)), val: 1},
    {_id: 1, date: new Date(Date.UTC(2003, 11, 27)), val: null},
    {_id: 2, date: new Date(Date.UTC(2003, 11, 26)), val: null},
    {_id: 3, date: new Date(Date.UTC(2003, 11, 25)), val: null},
    {_id: 4, date: new Date(Date.UTC(2003, 11, 24)), val: 5},
    {_id: 5, date: new Date(Date.UTC(2003, 11, 23)), val: null},
    {_id: 6, date: new Date(Date.UTC(2003, 11, 22)), val: null},
    {_id: 7, date: new Date(Date.UTC(2003, 11, 21)), val: null},
    {_id: 8, date: new Date(Date.UTC(2003, 11, 20)), val: 10},
    {_id: 9, date: new Date(Date.UTC(2003, 11, 19)), val: null},
    {_id: 10, date: new Date(Date.UTC(2003, 11, 18)), val: null},
    {_id: 11, date: new Date(Date.UTC(2003, 11, 17)), val: null},
    {_id: 12, date: new Date(Date.UTC(2003, 11, 16)), val: -5},
];

expected = [
    {_id: 12, date: ISODate("2003-12-16T00:00:00Z"), val: -5},
    {_id: 11, date: ISODate("2003-12-17T00:00:00Z"), val: -1.25},
    {_id: 10, date: ISODate("2003-12-18T00:00:00Z"), val: 2.5},
    {_id: 9, date: ISODate("2003-12-19T00:00:00Z"), val: 6.25},
    {_id: 8, date: ISODate("2003-12-20T00:00:00Z"), val: 10},
    {_id: 7, date: ISODate("2003-12-21T00:00:00Z"), val: 8.75},
    {_id: 6, date: ISODate("2003-12-22T00:00:00Z"), val: 7.5},
    {_id: 5, date: ISODate("2003-12-23T00:00:00Z"), val: 6.25},
    {_id: 4, date: ISODate("2003-12-24T00:00:00Z"), val: 5},
    {_id: 3, date: ISODate("2003-12-25T00:00:00Z"), val: 4},
    {_id: 2, date: ISODate("2003-12-26T00:00:00Z"), val: 3},
    {_id: 1, date: ISODate("2003-12-27T00:00:00Z"), val: 2},
    {_id: 0, date: ISODate("2003-12-28T00:00:00Z"), val: 1}
];

assert.commandWorked(coll.insert(collection));
result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {date: 1},
                     output: {val: {$linearFill: "$val"}},
                 }
             }])
             .toArray();
assert.eq(result, expected);
coll.drop();
// Test $linearFill with partitions.
collection = [
    {borough: "Manhattan", x: 1, y: -1},
    {borough: "Manhattan", x: 5, y: null},
    {borough: "Bronx", x: 1000, y: -10},
    {borough: "Bronx", x: 1100, y: null},
    {borough: "Bronx", x: 1200, y: null},
    {borough: "Bronx", x: 2000, y: 200},
    {borough: "Manhattan", x: 10, y: -10},
    {borough: "Queens", x: 2, y: -2},
    {borough: "Queens", x: 20, y: -20},
    {borough: "Queens", x: 3, y: null},
];
expected = [
    {borough: "Bronx", x: 1000, y: -10},
    {borough: "Bronx", x: 1100, y: 11},
    {borough: "Bronx", x: 1200, y: 32},
    {borough: "Bronx", x: 2000, y: 200},
    {borough: "Manhattan", x: 1, y: -1},
    {borough: "Manhattan", x: 5, y: -5},
    {borough: "Manhattan", x: 10, y: -10},
    {borough: "Queens", x: 2, y: -2},
    {borough: "Queens", x: 3, y: -3},
    {borough: "Queens", x: 20, y: -20}
];
assert.commandWorked(coll.insert(collection));

result = coll.aggregate([
                 {$project: {_id: 0}},
                 {
                     $setWindowFields: {
                         partitionBy: "$borough",
                         sortBy: {x: 1},
                         output: {y: {$linearFill: "$y"}},
                     }
                 }
             ])
             .toArray();
assert.eq(result, expected);

// Test sets of nulls with partitions.
collection = [
    {borough: "SI", x: 1, y: 1},
    {borough: "SI", x: 2, y: null},
    {borough: "SI", x: 3, y: null},
    {borough: "SI", x: 4, y: null},
    {borough: "SI", x: 5, y: 5},
    {borough: "SI", x: 6, y: null},
    {borough: "SI", x: 7, y: null},
    {borough: "SI", x: 8, y: null},
    {borough: "SI", x: 9, y: 10},
    {borough: "SI", x: 10, y: null},
    {borough: "SI", x: 11, y: null},
    {borough: "SI", x: 12, y: null},
    {borough: "SI", x: 13, y: -5},
];
assert.commandWorked(coll.insert(collection));
expected = [
    {borough: "Bronx", x: 1000, y: -10},   {borough: "Bronx", x: 1100, y: 11},
    {borough: "Bronx", x: 1200, y: 32},    {borough: "Bronx", x: 2000, y: 200},
    {borough: "Manhattan", x: 1, y: -1},   {borough: "Manhattan", x: 5, y: -5},
    {borough: "Manhattan", x: 10, y: -10}, {borough: "Queens", x: 2, y: -2},
    {borough: "Queens", x: 3, y: -3},      {borough: "Queens", x: 20, y: -20},
    {borough: "SI", x: 1, y: 1},           {borough: "SI", x: 2, y: 2},
    {borough: "SI", x: 3, y: 3},           {borough: "SI", x: 4, y: 4},
    {borough: "SI", x: 5, y: 5},           {borough: "SI", x: 6, y: 6.25},
    {borough: "SI", x: 7, y: 7.5},         {borough: "SI", x: 8, y: 8.75},
    {borough: "SI", x: 9, y: 10},          {borough: "SI", x: 10, y: 6.25},
    {borough: "SI", x: 11, y: 2.5},        {borough: "SI", x: 12, y: -1.25},
    {borough: "SI", x: 13, y: -5}
];

result = coll.aggregate([
                 {$project: {_id: 0}},
                 {
                     $setWindowFields: {
                         partitionBy: "$borough",
                         sortBy: {x: 1},
                         output: {y: {$linearFill: "$y"}},
                     }
                 }
             ])
             .toArray();
assert.eq(result, expected);
coll.drop();

// Test dates with partitions.
collection = [
    {borough: "Manhattan", y: -1, date: new Date(Date.UTC(2000, 11, 28))},
    {borough: "Manhattan", y: null, date: new Date(Date.UTC(2001, 11, 28))},
    {borough: "Bronx", y: -10, date: new Date(Date.UTC(2002, 11, 28))},
    {borough: "Bronx", y: null, date: new Date(Date.UTC(2003, 11, 28))},
    {borough: "Bronx", y: null, date: new Date(Date.UTC(2004, 11, 28))},
    {borough: "Bronx", y: 200, date: new Date(Date.UTC(2005, 11, 28))},
    {borough: "Manhattan", y: -10, date: new Date(Date.UTC(2006, 11, 28))},
    {borough: "Queens", y: -2, date: new Date(Date.UTC(2007, 11, 28))},
    {borough: "Queens", y: -20, date: new Date(Date.UTC(2008, 11, 28))},
    {borough: "Queens", y: null, date: new Date(Date.UTC(2009, 11, 28))},
];
assert.commandWorked(coll.insert(collection));
expected = [
    {borough: "Bronx", y: -10, date: ISODate("2002-12-28T00:00:00Z")},
    {borough: "Bronx", y: 59.93613138686132, date: ISODate("2003-12-28T00:00:00Z")},
    {borough: "Bronx", y: 130.06386861313868, date: ISODate("2004-12-28T00:00:00Z")},
    {borough: "Bronx", y: 200, date: ISODate("2005-12-28T00:00:00Z")},
    {borough: "Manhattan", y: -1, date: ISODate("2000-12-28T00:00:00Z")},
    {borough: "Manhattan", y: -2.499315381104519, date: ISODate("2001-12-28T00:00:00Z")},
    {borough: "Manhattan", y: -10, date: ISODate("2006-12-28T00:00:00Z")},
    {borough: "Queens", y: -2, date: ISODate("2007-12-28T00:00:00Z")},
    {borough: "Queens", y: -20, date: ISODate("2008-12-28T00:00:00Z")},
    {borough: "Queens", y: null, date: ISODate("2009-12-28T00:00:00Z")}
];

result = coll.aggregate([
                 {$project: {_id: 0}},
                 {
                     $setWindowFields: {
                         partitionBy: "$borough",
                         sortBy: {date: 1},
                         output: {y: {$linearFill: "$y"}},
                     }
                 }
             ])
             .toArray();
assert.eq(result, expected);

// If there are not enough values to perform interpolation, we output the original documents.
coll.drop();
collection = [
    {_id: 0, val: null},
    {_id: 1, val: null},
    {_id: 4, val: null},
    {_id: 9, val: null},
    {_id: 10, val: null},
];
assert.commandWorked(coll.insert(collection));
result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {_id: 1},
                     output: {val: {$linearFill: "$val"}},
                 }
             }])
             .toArray();
assert.eq(result, collection);

coll.drop();
collection = [
    {_id: 0, val: 0},
    {_id: 1, val: null},
    {_id: 4, val: null},
    {_id: 9, val: null},
    {_id: 10, val: null},
];
assert.commandWorked(coll.insert(collection));
result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {_id: 1},
                     output: {val: {$linearFill: "$val"}},
                 }
             }])
             .toArray();
assert.eq(result, collection);

coll.drop();
collection = [
    {_id: 0, val: null},
    {_id: 1, val: null},
    {_id: 4, val: null},
    {_id: 9, val: null},
    {_id: 10, val: 10},
    {_id: 11, val: null},
    {_id: 12, val: null},
    {_id: 13, val: 13},
];
expected = [
    {_id: 0, val: null},
    {_id: 1, val: null},
    {_id: 4, val: null},
    {_id: 9, val: null},
    {_id: 10, val: 10},
    {_id: 11, val: 11},
    {_id: 12, val: 12},
    {_id: 13, val: 13},
];
assert.commandWorked(coll.insert(collection));
result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {_id: 1},
                     output: {val: {$linearFill: "$val"}},
                 }
             }])
             .toArray();
assert.eq(result, expected);

// There can be no repeated values in the field we sort on.
coll.drop();
collection = [
    {test: 0, val: 0},
    {test: 1, val: null},
    {test: 9, val: 9},
    {test: 10, val: 10},
    {test: 10, val: null},
    {test: 11, val: 11}
];
assert.commandWorked(coll.insert(collection));

assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$project: {_id: 0}},
        {
            $setWindowFields: {
                sortBy: {test: 1},
                output: {val: {$linearFill: "$val"}},
            }
        }
    ],
    cursor: {}
}),
                             6050106);

// The sort field values must be of type numeric.
coll.drop();
collection = [
    {test: null, val: 0},
    {test: 1, val: null},
    {test: 4, val: null},
    {test: 9, val: null},
    {test: 10, val: 10},
];
assert.commandWorked(coll.insert(collection));
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$project: {_id: 0}},
        {
            $setWindowFields: {
                sortBy: {test: 1},
                output: {val: {$linearFill: "$val"}},
            }
        }
    ],
    cursor: {}
}),
                             ErrorCodes.TypeMismatch);

// The field we are filling can only have a numeric or null value.
coll.drop();
collection = [
    {test: 0, val: "str", z: 2},
    {test: 1, val: null, z: 3},
    {test: 9, val: 9, z: 4},
    {test: 10, val: "str", z: 50},
    {test: 10, val: null, z: 9},
    {test: 11, val: 11, z: 2}
];
assert.commandWorked(coll.insert(collection));
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$project: {_id: 0}},
        {
            $setWindowFields: {
                sortBy: {test: 1},
                output: {val: {$linearFill: "$val"}},
            }
        }
    ],
    cursor: {}
}),
                             ErrorCodes.TypeMismatch);

// There can only be a single sort key for $linearFill.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$project: {_id: 0}},
        {
            $setWindowFields: {
                sortBy: {test: 1, z: 1},
                output: {val: {$linearFill: "$val"}},
            }
        }
    ],
    cursor: {}
}),
                             605001);
// Mixing dates with numerics in sort field is not allowed.
coll.drop();
collection = [{x: 1, y: 10}, {x: 2, y: null}, {x: new Date(), y: 20}];
assert.commandWorked(coll.insert(collection));
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$project: {_id: 0}},
        {
            $setWindowFields: {
                sortBy: {x: 1},
                output: {y: {$linearFill: "$y"}},
            }
        }
    ],
    cursor: {}
}),
                             ErrorCodes.TypeMismatch);
})();
