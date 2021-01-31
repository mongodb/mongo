// Confirms that the mapReduce reduce function will process data sets larger than 16MB.
// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   requires_fastcount,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";
const coll = db.mr_bigobject;
coll.drop();
const outputColl = db.mr_bigobject_out;
outputColl.drop();

const largeString = Array.from({length: 6 * 1024 * 1024}, _ => "a").join("");

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 5; i++)
    bulk.insert({_id: i, s: largeString});
assert.commandWorked(bulk.execute());

// MapReduce succeeds when the reduce function processes single-key data sets larger than 16MB.
const mapFn = function() {
    emit(1, this.s);
};

let reduceFn = function(k, v) {
    return 1;
};

assert.commandWorked(coll.mapReduce(mapFn, reduceFn, {out: {"merge": outputColl.getName()}}));
assert.eq([{_id: 1, value: 1}], outputColl.find().toArray());

// The reduce function processes the expected amount of data.
reduceFn = function(k, v) {
    total = 0;
    for (let i = 0; i < v.length; i++) {
        const x = v[i];
        if (typeof (x) == "number")
            total += x;
        else
            total += x.length;
    }
    return total;
};

assert.commandWorked(coll.mapReduce(mapFn, reduceFn, {out: {"merge": outputColl.getName()}}));
assert.eq([{_id: 1, value: coll.count() * largeString.length}], outputColl.find().toArray());
}());
