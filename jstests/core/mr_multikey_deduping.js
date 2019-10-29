// Tests that the presence of an index does not impact the results of a mapReduce
// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]
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

    let res =
        assert.commandWorked(coll.mapReduce(mapFn, reduceFn, {out: outColl.getName(), query: {}}));
    assert.eq(1, res.counts.input);
    assert(outColl.drop());

    res = assert.commandWorked(
        coll.mapReduce(mapFn, reduceFn, {out: outColl.getName(), query: {arr: {$gte: 0}}}));
    assert.eq(1, res.counts.input);
    assert(outColl.drop());

    // Now test that we get the same results when there's an index present.
    assert.commandWorked(coll.ensureIndex({arr: 1}));
    res = assert.commandWorked(
        coll.mapReduce(mapFn, reduceFn, {out: outColl.getName(), query: {arr: {$gte: 0}}}));
    assert.eq(1, res.counts.input);
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
                {mapreduce: coll.getName(), map: mapFn, reduce: reduceFn, out: {inline: true}}))
            .results;
    const resultsNoIndexEqualityOnName = assert
                                             .commandWorked(db.runCommand({
                                                 mapreduce: coll.getName(),
                                                 map: mapFn,
                                                 reduce: reduceFn,
                                                 query: {name: 'name1'},
                                                 out: {inline: true}
                                             }))
                                             .results;
    const resultsNoIndexRangeOnName = assert
                                          .commandWorked(db.runCommand({
                                              mapreduce: coll.getName(),
                                              map: mapFn,
                                              reduce: reduceFn,
                                              query: {name: {$gt: 'name'}},
                                              out: {inline: true}
                                          }))
                                          .results;

    assert.eq([{_id: "cat", value: 3}, {_id: "dog", value: 2}, {_id: "mouse", value: 1}],
              resultsNoIndexNoQuery);
    assert.eq([{_id: "cat", value: 1}, {_id: "dog", value: 1}], resultsNoIndexEqualityOnName);
    assert.eq(resultsNoIndexNoQuery, resultsNoIndexRangeOnName);

    assert.commandWorked(coll.ensureIndex({name: 1, tags: 1}));

    const resultsIndexedNoQuery =
        assert
            .commandWorked(db.runCommand(
                {mapreduce: coll.getName(), map: mapFn, reduce: reduceFn, out: {inline: true}}))
            .results;
    const resultsIndexedEqualityOnName = assert
                                             .commandWorked(db.runCommand({
                                                 mapreduce: coll.getName(),
                                                 map: mapFn,
                                                 reduce: reduceFn,
                                                 query: {name: 'name1'},
                                                 out: {inline: true}
                                             }))
                                             .results;
    const resultsIndexedRangeOnName = assert
                                          .commandWorked(db.runCommand({
                                              mapreduce: coll.getName(),
                                              map: mapFn,
                                              reduce: reduceFn,
                                              query: {name: {$gt: 'name'}},
                                              out: {inline: true}
                                          }))
                                          .results;

    assert.eq(resultsNoIndexNoQuery, resultsIndexedNoQuery);
    assert.eq(resultsNoIndexEqualityOnName, resultsIndexedEqualityOnName);
    assert.eq(resultsNoIndexRangeOnName, resultsIndexedRangeOnName);
}());
}());
