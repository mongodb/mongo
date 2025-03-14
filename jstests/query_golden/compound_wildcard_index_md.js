/**
 * Tests that we generate correct IXSCANs using compound wildcard indexes.
 */
import {normalizeArray} from "jstests/libs/golden_test.js";
import {code, linebreak, section, subSection} from "jstests/libs/pretty_md.js";
import {getPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

/**
 * Takes a collection and a match expression. Outputs the match expression, the results and the
 * index scans of its winning plan to markdown.
 */
export function outputIndexScansAndResults(coll, query) {
    const results = coll.find(query).toArray();
    const explain = coll.find(query).explain();

    subSection("Query");
    code(tojson(query));

    subSection("Results");
    code(normalizeArray(results));

    subSection("Index Scans");
    const ixScans = getPlanStages(getWinningPlanFromExplain(explain), "IXSCAN")
                        .map(ixScan => ({
                                 indexName: ixScan.indexName,
                                 indexBounds: ixScan.indexBounds,
                                 keyPattern: ixScan.keyPattern
                             }))
                        .sort((a, b) => a.indexName.localeCompare(b.indexName));
    code(tojson(ixScans));
    linebreak();
}

const coll = db[jsTestName()];

assert(coll.drop());
assert.commandWorked(coll.createIndexes([{"sub.$**": 1, num: 1}]));
assert.commandWorked(coll.insertMany([
    {_id: 0, num: 1, sub: {num: 5}},
    {_id: 1, num: 100, sub: {num: 5}},
    {_id: 2, num: 1, sub: {num: 0}},
    {_id: 3, num: 1, sub: [{num: 5}]},
    {_id: 4, num: 1, sub: [{num: 0}]},
    {_id: 5, num: 1, sub: {num: [5]}},
    {_id: 11, str: "1", sub: {num: 9}},
    {_id: 12, str: "not_matching", sub: {num: 9}},
    {_id: 13, str: "1", sub: {num: 11}},
]));

section("Test a filter using only the wildcard prefix of the CWI");
outputIndexScansAndResults(coll, {"sub.num": {$gt: 4}});

section("Test a filter using both components of the CWI");
outputIndexScansAndResults(coll, {num: 1, "sub.num": {$gt: 4}});

section("Test an $elemMatch (object) filter using both components of the CWI");
outputIndexScansAndResults(coll, {num: 1, sub: {$elemMatch: {num: {$gt: 4}}}});

section("Test an $elemMatch (value) filter using both components of the CWI");
outputIndexScansAndResults(coll, {num: 1, "sub.num": {$elemMatch: {$gt: 4}}});

section("Test a filter using only the non-wildcard prefix of the CWI");
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndexes([{num: 1, "sub.$**": 1}, {str: 1, "sub.$**": 1}]));
outputIndexScansAndResults(coll, {num: 1});

section("Test a filter using both components of the CWI");
outputIndexScansAndResults(coll, {num: 1, "sub.num": {$gt: 4}});

section("Test an $or filter using both components of the CWI");
assert.commandWorked(coll.createIndex({"sub.num": 1}));
outputIndexScansAndResults(coll, {$or: [{num: 1}, {"sub.num": {$gt: 4}}]});
assert.commandWorked(coll.dropIndex({"sub.num": 1}));

section("Test a filter with nested $and under a $or");
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndexes([{num: 1, "sub.$**": 1}, {str: 1, "sub.$**": 1}]));
outputIndexScansAndResults(coll, {
    $or: [
        {num: 1, "sub.num": {$gt: 4}},
        {str: '1', "sub.num": {$lt: 10}},
    ],
});

section("Test a filter with nested $or under an $and");
assert.commandWorked(coll.createIndex({"sub.num": 1}));
outputIndexScansAndResults(coll, {
    $and: [
        {$or: [{num: 1}, {"sub.num": {$gt: 4}}]},
        {$or: [{str: '1'}, {"sub.num": {$lt: 10}}]},
    ],
});

section("Test the query reproducing SERVER-95374");
assert(coll.drop());
assert.commandWorked(coll.insertMany([
    {_id: 0, str: "myRegex", num: 0, z: 0},
    {_id: 1, str: "not_matching", num: 100, z: 1},
    {_id: 2, str: "not_matching", num: 0, z: 1},
    {_id: 3, str: "myRegex", num: 100, z: 1},
    {_id: 11, str: "not_matching", num: 0, z: 100},
    {_id: 12, str: "myRegex", num: 100, z: 100},
]));
assert.commandWorked(coll.createIndex({str: 1, "$**": 1}, {wildcardProjection: {str: 0}}));
assert.commandWorked(coll.createIndex({z: 1}));
outputIndexScansAndResults(coll, {
    $or: [
        {$and: [{str: /myRegex/}, {num: {$lte: 0}}]},
        {z: 1},
    ]
});