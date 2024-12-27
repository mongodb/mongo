/**
 * Make sure that $gt and $lt queries return the same results regardless of whether there is a
 * multikey index.
 * @tags: [
 *     requires_fcv_81,
 *     requires_getmore
 *  ]
 */

import {arrayDiff, arrayEq} from "jstests/aggregation/extras/utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    getWinningPlanFromExplain,
    isCollscan,
    isIxscanMultikey
} from "jstests/libs/query/analyze_plan.js";

function buildErrorString(q, indexed, nonIndexed) {
    const arrDiff = arrayDiff(indexed, nonIndexed);
    if (arrDiff === false) {
        return "";
    }
    let errStr = "Ran query " + tojson(q) +
        " and got mismatched results.\nUnmatched from indexed collection (" + arrDiff.left.length +
        "/" + indexed.length + "): " + tojson(arrDiff.left) + "\nUnmatched from nonIndexed (" +
        arrDiff.right.length + "/" + nonIndexed.length + "): " + tojson(arrDiff.right);
    return errStr;
}
const coll = assertDropAndRecreateCollection(db, "array_index_and_nonIndex_consistent");

assert.commandWorked(coll.createIndex({val: 1}));

const singleValues = [
    [1, 2],      [3, 4], [3, 1],   {"test": 5}, [{"test": 7}], [true, false], 2,        3,
    4,           [2],    [3],      [4],         [1, true],     [true, 1],     [1, 4],   [null],
    null,        MinKey, [MinKey], [MinKey, 3], [3, MinKey],   MaxKey,        [MaxKey], [MaxKey, 3],
    [3, MaxKey], []
];
const nestedValues = singleValues.map(value => [value]);
const doubleNestedValues = nestedValues.map(value => [value]);
const insertDocs =
    singleValues.concat(nestedValues).concat(doubleNestedValues).map(value => ({val: value}));

assert.commandWorked(coll.insert(insertDocs));

assert.eq(
    coll.find({}).toArray().length, singleValues.length * 3, "Wrong number of documents found!");

const flattenPredicates = [
    [2, 2],      [0, 3],      [3, 0],      [1, 3],      [3, 1],       [1, 5],      [5, 1], [1],
    [3],         [5],         {"test": 2}, {"test": 6}, [true, true], [true],      true,   1,
    3,           5,           [],          [MinKey],    [MinKey, 2],  [MinKey, 4], MinKey, [MaxKey],
    [MaxKey, 2], [MaxKey, 4], MaxKey,      [],          false,        null,        [null],
];
const nestedPredicates = flattenPredicates.map(predicate => [predicate]);
const doubleNestedPreds = nestedPredicates.map(predicate => [predicate]);
const queryList = flattenPredicates.concat(nestedPredicates).concat(doubleNestedPreds);

assert.eq(queryList.length, flattenPredicates.length * 3, "Wrong number of predicates");

queryList.forEach(function(q) {
    const queryPreds = [
        {$lt: q},
        {$lte: q},
        {$gt: q},
        {$gte: q},
        {$eq: q},
        {$in: [q, "nonExistentString"]},
        {$not: {$lt: q}},
        {$not: {$lte: q}},
        {$not: {$gt: q}},
        {$not: {$gte: q}},
        {$not: {$eq: q}},
        {$not: {$in: [q, "nonExistentString"]}},
        {$elemMatch: {$not: {$eq: q}}},
        {$elemMatch: {$not: {$gte: q}}},
        {$elemMatch: {$not: {$lte: q}}},
    ].map(predicate => ({predicate, isArray: Array.isArray(q)}));
    const projOutId = {_id: 0, val: 1};
    queryPreds.forEach(function(pred) {
        const query = {val: pred.predicate};
        const indexRes = coll.find(query, projOutId).toArray().sort();
        if (pred.isArray && !pred.predicate.$elemMatch && !pred.predicate.$not) {
            // This is a non negative query against an array. It should use the index.
            const indexPlan = coll.find(query, projOutId).explain();
            assert(isIxscanMultikey(getWinningPlanFromExplain(indexPlan)), indexPlan);
        }
        const nonIndexedRes = coll.find(query, projOutId).hint({$natural: 1}).toArray().sort();
        const nonIndexedPlan = coll.find(query, projOutId).hint({$natural: 1}).explain();
        assert(isCollscan(db, nonIndexedPlan), nonIndexedPlan);
        assert(arrayEq(indexRes, nonIndexedRes), buildErrorString(query, indexRes, nonIndexedRes));
    });
});
// Test queries with multiple intervals.
const multiIntQueryList = [
    {val: {$gt: 1, $lt: 3}},
    {val: {$gt: [1], $lt: [3]}},
    {val: {$not: {$gt: 3, $lt: 1}}},
    {val: {$not: {$gt: [3], $lt: [1]}}},
    {val: {$not: {$gt: [3], $lt: 1}}},
    {val: {$not: {$not: {$lt: 3}}}},
    {val: {$not: {$not: {$lt: true}}}},
    {val: {$not: {$not: {$lt: [true]}}}},
    {val: {$not: {$not: {$lt: [3]}}}},
];
multiIntQueryList.forEach(function(q) {
    const projOutId = {_id: 0, val: 1};
    const indexRes = coll.find(q, projOutId).toArray().sort();
    const nonIndexedRes = coll.find(q, projOutId).hint({$natural: 1}).toArray().sort();
    assert(arrayEq(indexRes, nonIndexedRes), buildErrorString(q, indexRes, nonIndexedRes));
});
