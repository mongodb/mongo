// Tests a mapReduce for a relatively simple use case of computing an average. This is interesting
// because it does more than a simple sum for the reduce function and also needs to use a finalize
// function to do the final division to compute the average from the sum and totals.
//
// Cannot implicitly shard accessed collections because mapReduce cannot replace a sharded
// collection as output.
// @tags: [
//   assumes_unsharded_collection,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   sbe_incompatible,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";

const coll = db.mr_blog_posts;
coll.drop();

assert.commandWorked(coll.insert([
    {
        _id: "blog 1",
        author: "x",
        comments: [{user_id: "a", txt: "asdasdasd"}, {user_id: "b", txt: "asdasdasdasdasdasdas"}]
    },
    {
        _id: "blog 2",
        author: "y",
        comments: [{user_id: "b", txt: "asdasdasdaaa"}, {user_id: "c", txt: "asdasdasdaasdasdas"}]
    }
]));

function mapFn() {
    for (let comment of this.comments) {
        emit(comment.user_id, {totalSize: comment.txt.length, num: 1});
    }
}

function reduceFn(user_id, values) {
    let reduced = {totalSize: 0, num: 0};
    for (let value of values) {
        reduced.totalSize += value.totalSize;
        reduced.num += value.num;
    }
    return reduced;
}

const outputColl = db.mr_compute_avg_out;
outputColl.drop();

function reformat(cmdResult) {
    let x = {};
    let cursor;
    if (cmdResult.results)
        cursor = cmdResult.results;
    else
        cursor = outputColl.find();
    cursor.forEach(result => {
        x[result._id] = result.value;
    });
    return x;
}

function finalizeFn(user_id, res) {
    res.avg = res.totalSize / res.num;
    return res;
}

let res = coll.mapReduce(mapFn, reduceFn, {finalize: finalizeFn, out: outputColl.getName()});
assert.commandWorked(res);
let resultAsSingleObj = reformat(res);
assert.eq(9, resultAsSingleObj.a.avg, () => tojson(resultAsSingleObj));
assert.eq(16, resultAsSingleObj.b.avg, () => tojson(resultAsSingleObj));
assert.eq(18, resultAsSingleObj.c.avg, () => tojson(resultAsSingleObj));
outputColl.drop();

res = coll.mapReduce(mapFn, reduceFn, {finalize: finalizeFn, out: {inline: 1}});
assert.commandWorked(res);
resultAsSingleObj = reformat(res);
assert.eq(9, resultAsSingleObj.a.avg, () => tojson(resultAsSingleObj));
assert.eq(16, resultAsSingleObj.b.avg, () => tojson(resultAsSingleObj));
assert.eq(18, resultAsSingleObj.c.avg, () => tojson(resultAsSingleObj));
outputColl.drop();
assert(!("result" in res), () => `Expected inline output with 'results': ${tojson(res)}`);

res = coll.mapReduce(mapFn, reduceFn, {finalize: finalizeFn, out: {inline: 1}});
assert.commandWorked(res);
resultAsSingleObj = reformat(res);
assert.eq(9, resultAsSingleObj.a.avg, () => tojson(resultAsSingleObj));
assert.eq(16, resultAsSingleObj.b.avg, () => tojson(resultAsSingleObj));
assert.eq(18, resultAsSingleObj.c.avg, () => tojson(resultAsSingleObj));
assert(!("result" in res), () => `Expected inline output with 'results': ${tojson(res)}`);
}());
