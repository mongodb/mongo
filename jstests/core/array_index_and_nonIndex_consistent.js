/**
 * Make sure that $gt and $lt queries return the same results regardless of whether there is a
 * multikey index.
 * @tags: [
 *   requires_fcv_47,
 *   sbe_incompatible,
 * ]
 */

(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // arrayEq

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
const indexColl = db.indexColl;
const nonIndexedColl = db.nonIndexedColl;
indexColl.drop();
nonIndexedColl.drop();

db.indexColl.createIndex({val: 1});
const collList = [indexColl, nonIndexedColl];

collList.forEach(function(collObj) {
    assert.commandWorked(collObj.insert([
        {val: [1, 2]},
        {val: [3, 4]},
        {val: [3, 1]},
        {val: {"test": 5}},
        {val: [{"test": 7}]},
        {val: [true, false]},
        {val: 2},
        {val: 3},
        {val: 4},
        {val: [2]},
        {val: [3]},
        {val: [4]},
        {val: [1, true]},
        {val: [true, 1]},
        {val: [1, 4]},
        {val: [null]},
        {val: null},
        {val: MinKey},
        {val: [MinKey]},
        {val: [MinKey, 3]},
        {val: [3, MinKey]},
        {val: MaxKey},
        {val: [MaxKey]},
        {val: [MaxKey, 3]},
        {val: [3, MaxKey]},
        {val: []},
    ]));
});

const queryList = [
    [2, 2],      [0, 3],      [3, 0],      [1, 3],      [3, 1],       [1, 5],      [5, 1], [1],
    [3],         [5],         {"test": 2}, {"test": 6}, [true, true], [true],      true,   1,
    3,           5,           [],          [MinKey],    [MinKey, 2],  [MinKey, 4], MinKey, [MaxKey],
    [MaxKey, 2], [MaxKey, 4], MaxKey,      [],          false,        null,        [null],
];

queryList.forEach(function(q) {
    const queryPreds = [
        {$lt: q},
        {$lte: q},
        {$gt: q},
        {$gte: q},
        {$eq: q},
        {$not: {$lt: q}},
        {$not: {$lte: q}},
        {$not: {$gt: q}},
        {$not: {$gte: q}},
        {$not: {$eq: q}},
    ];
    const projOutId = {_id: 0, val: 1};
    queryPreds.forEach(function(pred) {
        const query = {val: pred};
        const indexRes = indexColl.find(query, projOutId).sort({val: 1}).toArray();
        const nonIndexedRes = nonIndexedColl.find(query, projOutId).sort({val: 1}).toArray();
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
    const indexRes = indexColl.find(q, projOutId).sort({val: 1}).toArray();
    const nonIndexedRes = nonIndexedColl.find(q, projOutId).sort({val: 1}).toArray();
    assert(arrayEq(indexRes, nonIndexedRes), buildErrorString(q, indexRes, nonIndexedRes));
});
})();
