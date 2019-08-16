// Cannot implicitly shard accessed collections because of following errmsg: Cannot output to a
// non-sharded collection because sharded collection exists already.
// @tags: [
//   assumes_against_mongod_not_mongos,
//   assumes_unsharded_collection,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
// ]

(function() {
"using strict";

const assertCounts = countsObj => {
    assert.eq(Object.keys(countsObj).length, 4);
    assert.eq(countsObj.input, 0);
    assert.eq(countsObj.emit, 0);
    assert.eq(countsObj.reduce, 0);
    assert.eq(countsObj.output, 0);
};

const assertTiming = timingObj => {
    print(tojson(timingObj));
    assert.eq(Object.keys(timingObj).length, 4);
    assert(isNumber(timingObj.mapTime));
    assert(isNumber(timingObj.emitLoop));
    assert(isNumber(timingObj.reduceTime));
    assert(isNumber(timingObj.total));
};

const assertInlineOutputFormat = res => {
    assert.commandWorked(res);
    assert(Array.isArray(res.results));
    assert(!res.result);
    assert(isNumber(res.timeMillis));
    assertCounts(res.counts);
    assert(!res.timing);
};

const assertCollectionOutputFormat = (res, outColl, outDb = null) => {
    res = coll.runCommand(cmd);
    assert.commandWorked(res);
    assert(!res.results);
    if (outDb) {
        assert.eq(res.result.db, outDb);
        assert.eq(res.result.collection, outColl);
    } else {
        assert.eq(res.result, outColl);
    }
    assert(isNumber(res.timeMillis));
    assertCounts(res.counts);
    assert(!res.timing);
};

const assertVerboseFormat = res => {
    assert.commandWorked(res);
    assert(Array.isArray(res.results));
    assert(!res.result);
    assert(isNumber(res.timeMillis));
    assertCounts(res.counts);
    assert(res.timing);
    assertTiming(res.timing);
};

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryUseAggMapReduce: true}));
const coll = db.mr_output_conversion;
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= 100; ++i) {
    if (i % 3 === 0) {
        bulk.insert({user: `user_${i}`, colour: 'red'});
    } else if (i % 3 === 1) {
        bulk.insert({user: `user_${i}`, colour: 'blue'});
    } else {
        bulk.insert({user: `user_${i}`});
    }
}
assert.commandWorked(bulk.execute());

const map = () => {
    emit(this.colour, 1);
};
const reduce = (k, v) => {
    return Array.sum(v);
};

// Inline should produce results array.
let cmd = {
    mapReduce: coll.getName(),
    map,
    reduce,
    out: {inline: 1},
};
let res = coll.runCommand(cmd);
assertInlineOutputFormat(res);

// Specifying output collection should produce {result: <collName>}
cmd = {
    mapReduce: coll.getName(),
    map,
    reduce,
    out: coll.getName()
};
assertCollectionOutputFormat(res, coll.getName());

// Specifying output collection and db should produce
// {result:{ db: <dbName>, collection:  <collName> }}
cmd = {
    mapReduce: coll.getName(),
    map,
    reduce,
    out: {replace: coll.getName(), db: coll.getDB().getName()}
};
assertCollectionOutputFormat(res, coll.getName(), coll.getDB().getName());

// Timing information should be present with verbose option.
cmd = {
    mapReduce: coll.getName(),
    map,
    reduce,
    out: {inline: 1},
    verbose: true
};
res = coll.runCommand(cmd);
assertVerboseFormat(res);

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryUseAggMapReduce: false}));
})();
