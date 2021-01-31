// Tests that the presence of an index does not impact the results of a mapReduce
// @tags: [
//   # MR commands may not see previous inserts because MR does not support causal consistency so we
//   # add this tag to exclude transactional passthroughs which commit versions in an ascynchronos
//   # fashion and can cause stale reads.
//   assumes_unsharded_collection,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]

load("jstests/aggregation/extras/utils.js");  // For resultsEq
(function() {
"use strict";

const coll = db.mr_multikey_deduping;
coll.drop();
const outColl = db.mr_multikey_deduping_out;
outColl.drop();

(function testSingleKeyIndex() {
    assert.commandWorked(coll.insert({arr: [1, 2]}));

    const mapFn = function() {
        emit(this._id, 1);
    };
    const reduceFn = function(k, vals) {
        return Array.sum(vals);
    };

    let res = assert.commandWorked(
        coll.mapReduce(mapFn, reduceFn, {out: {merge: outColl.getName()}, query: {}}));
    assert(outColl.drop());

    res = assert.commandWorked(coll.mapReduce(
        mapFn, reduceFn, {out: {merge: outColl.getName()}, query: {arr: {$gte: 0}}}));
    assert(outColl.drop());

    // Now test that we get the same results when there's an index present.
    assert.commandWorked(coll.createIndex({arr: 1}));
    res = assert.commandWorked(coll.mapReduce(
        mapFn, reduceFn, {out: {merge: outColl.getName()}, query: {arr: {$gte: 0}}}));
    assert(outColl.drop());
}());

(function testCompoundIndex() {
    coll.drop();
    assert.commandWorked(coll.insert([
        {_id: 1, name: 'name1', tags: ['dog', 'cat']},
        {_id: 2, name: 'name2', tags: ['cat']},
        {_id: 3, name: 'name3', tags: ['mouse', 'cat', 'dog']},
        {_id: 4, name: 'name4', tags: []}
    ]));

    const mapFn = function() {
        for (var i = 0; i < this.tags.length; i++)
            emit(this.tags[i], 1);
    };

    const reduceFn = function(key, values) {
        return Array.sum(values);
    };

    const resultsNoIndexNoQuery =
        assert
            .commandWorked(db.runCommand(
                {mapreduce: coll.getName(), map: mapFn, reduce: reduceFn, out: {inline: 1}}))
            .results;
    const resultsNoIndexEqualityOnName = assert
                                             .commandWorked(db.runCommand({
                                                 mapreduce: coll.getName(),
                                                 map: mapFn,
                                                 reduce: reduceFn,
                                                 query: {name: 'name1'},
                                                 out: {inline: 1}
                                             }))
                                             .results;
    const resultsNoIndexRangeOnName = assert
                                          .commandWorked(db.runCommand({
                                              mapreduce: coll.getName(),
                                              map: mapFn,
                                              reduce: reduceFn,
                                              query: {name: {$gt: 'name'}},
                                              out: {inline: 1}
                                          }))
                                          .results;

    assert(resultsEq([{_id: "cat", value: 3}, {_id: "dog", value: 2}, {_id: "mouse", value: 1}],
                     resultsNoIndexNoQuery));
    assert(
        resultsEq([{_id: "cat", value: 1}, {_id: "dog", value: 1}], resultsNoIndexEqualityOnName));
    assert(resultsEq(resultsNoIndexNoQuery, resultsNoIndexRangeOnName));

    assert.commandWorked(coll.createIndex({name: 1, tags: 1}));

    const resultsIndexedNoQuery =
        assert
            .commandWorked(db.runCommand(
                {mapreduce: coll.getName(), map: mapFn, reduce: reduceFn, out: {inline: 1}}))
            .results;
    const resultsIndexedEqualityOnName = assert
                                             .commandWorked(db.runCommand({
                                                 mapreduce: coll.getName(),
                                                 map: mapFn,
                                                 reduce: reduceFn,
                                                 query: {name: 'name1'},
                                                 out: {inline: 1}
                                             }))
                                             .results;
    const resultsIndexedRangeOnName = assert
                                          .commandWorked(db.runCommand({
                                              mapreduce: coll.getName(),
                                              map: mapFn,
                                              reduce: reduceFn,
                                              query: {name: {$gt: 'name'}},
                                              out: {inline: 1}
                                          }))
                                          .results;

    assert(resultsEq(resultsNoIndexNoQuery, resultsIndexedNoQuery));
    assert(resultsEq(resultsNoIndexEqualityOnName, resultsIndexedEqualityOnName));
    assert(resultsEq(resultsNoIndexRangeOnName, resultsIndexedRangeOnName));
}());
}());
