load('jstests/aggregation/extras/utils.js');
load('jstests/libs/analyze_plan.js');  // For planHasStage.

// Use this for aggregations that only have arrays or results of specified order.
// It will check that cursors return the same results as non-cursors.
function aggregateOrdered(coll, pipeline) {
    var cursor = coll.aggregate(pipeline).toArray();
    var noCursor = coll.runCommand('aggregate', {pipeline: pipeline}).result;
    assert.eq(cursor, noCursor);
    return cursor;
}

// Use this for aggregations that have arrays or results of unspecified order.
// It will bypass the check that cursors return the same results as non-cursors.
function aggregateNoOrder(coll, pipeline) {
    return coll.aggregate(pipeline).toArray();
}

jsTestLog("Creating sharded cluster");
var shardedAggTest =
    new ShardingTest({shards: 2, mongos: 1, other: {chunkSize: 1, enableBalancer: true}});

jsTestLog("Setting up sharded cluster");
shardedAggTest.adminCommand({enablesharding: "aggShard"});
db = shardedAggTest.getDB("aggShard");
assert.commandWorked(db.adminCommand({setParameter: 1, logComponentVerbosity: {network: 0}}));
shardedAggTest.ensurePrimaryShard('aggShard', 'shard0000');

/* make sure its cleaned up */
db.ts1.drop();
db.literal.drop();

shardedAggTest.adminCommand({shardcollection: "aggShard.ts1", key: {"_id": 1}});
shardedAggTest.adminCommand({shardcollection: "aggShard.literal", key: {"_id": 1}});

/*
Test combining results in mongos for operations that sub-aggregate on shards.

The unusual operators here are $avg, $pushToSet, $push.   In the case of $avg,
the shard pipeline produces an object with the current subtotal and item count
so that these can be combined in mongos by totalling the subtotals counts
before performing the final division.  For $pushToSet and $push, the shard
pipelines produce arrays, but in mongos these are combined rather than simply
being added as arrays within arrays.
*/

var count = 0;
var strings = [
    "one",     "two",     "three",     "four",     "five",     "six",      "seven",
    "eight",   "nine",    "ten",       "eleven",   "twelve",   "thirteen", "fourteen",
    "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty"
];

jsTestLog("Bulk inserting data");
var nItems = 200000;
var bulk = db.ts1.initializeUnorderedBulkOp();
for (i = 1; i <= nItems; ++i) {
    bulk.insert({
        _id: i,
        counter: ++count,
        number: strings[i % 20],
        random: Math.random(),
        filler: "0123456789012345678901234567890123456789"
    });
}
assert.writeOK(bulk.execute());

// Turn on exception tracing in mongod to figure out exactly where the SCEs are coming from
// TEMPORARY - REMOVE ONCE SERVER-9622 IS RESOLVED
var config = db.getMongo().getDB("config");
var shards = config.shards.find().toArray();

jsTest.log("Tracing all exceptions in mongod...");
for (var i = 0; i < shards.length; i++) {
    var shardConn = new Mongo(shards[i].host);
    printjson(shardConn.getDB("admin").runCommand({setParameter: 1, traceExceptions: true}));
}

jsTestLog('a project and group in shards, result combined in mongos');
var a1 = aggregateNoOrder(db.ts1, [
    {$project: {cMod10: {$mod: ["$counter", 10]}, number: 1, counter: 1}},
    {$group: {_id: "$cMod10", numberSet: {$addToSet: "$number"}, avgCounter: {$avg: "$cMod10"}}},
    {$sort: {_id: 1}}
]);

for (i = 0; i < 10; ++i) {
    assert.eq(a1[i].avgCounter, a1[i]._id, 'agg sharded test avgCounter failed');
    assert.eq(a1[i].numberSet.length, 2, 'agg sharded test numberSet length failed');
}

jsTestLog('an initial group starts the group in the shards, and combines them in mongos');
var a2 = aggregateOrdered(db.ts1, [{$group: {_id: "all", total: {$sum: "$counter"}}}]);

jsTestLog('sum of an arithmetic progression S(n) = (n/2)(a(1) + a(n));');
assert.eq(a2[0].total, (nItems / 2) * (1 + nItems), 'agg sharded test counter sum failed');

jsTestLog('A group combining all documents into one, averaging a null field.');
assert.eq(aggregateOrdered(db.ts1, [{$group: {_id: null, avg: {$avg: "$missing"}}}]),
          [{_id: null, avg: null}]);

jsTestLog('an initial group starts the group in the shards, and combines them in mongos');
var a3 =
    aggregateOrdered(db.ts1, [{$group: {_id: "$number", total: {$sum: 1}}}, {$sort: {_id: 1}}]);

for (i = 0; i < strings.length; ++i) {
    assert.eq(a3[i].total, nItems / strings.length, 'agg sharded test sum numbers failed');
}

jsTestLog('a match takes place in the shards; just returning the results from mongos');
var a4 = aggregateNoOrder(db.ts1, [{
                              $match: {
                                  $or: [
                                      {counter: 55},
                                      {counter: 1111},
                                      {counter: 2222},
                                      {counter: 33333},
                                      {counter: 99999},
                                      {counter: 55555}
                                  ]
                              }
                          }]);

assert.eq(a4.length, 6, tojson(a4));
for (i = 0; i < 6; ++i) {
    c = a4[i].counter;
    printjson({c: c});
    assert((c == 55) || (c == 1111) || (c == 2222) || (c == 33333) || (c == 99999) || (c == 55555),
           'agg sharded test simple match failed');
}

function testSkipLimit(ops, expectedCount) {
    jsTestLog('testSkipLimit(' + tojson(ops) + ', ' + expectedCount + ')');
    if (expectedCount > 10) {
        // make shard -> mongos intermediate results less than 16MB
        ops.unshift({$project: {_id: 1}});
    }

    ops.push({$group: {_id: 1, count: {$sum: 1}}});

    var out = aggregateOrdered(db.ts1, ops);
    assert.eq(out[0].count, expectedCount);
}

testSkipLimit([], nItems);  // control
testSkipLimit([{$skip: 10}], nItems - 10);
testSkipLimit([{$limit: 10}], 10);
testSkipLimit([{$skip: 5}, {$limit: 10}], 10);
testSkipLimit([{$limit: 10}, {$skip: 5}], 10 - 5);
testSkipLimit([{$skip: 5}, {$skip: 3}, {$limit: 10}], 10);
testSkipLimit([{$skip: 5}, {$limit: 10}, {$skip: 3}], 10 - 3);
testSkipLimit([{$limit: 10}, {$skip: 5}, {$skip: 3}], 10 - 3 - 5);

// test sort + limit (using random to pull from both shards)
function testSortLimit(limit, direction) {
    jsTestLog('testSortLimit(' + limit + ', ' + direction + ')');
    shardedAggTest.stopBalancer();  // TODO: remove after fixing SERVER-9622
    var from_cursor =
        db.ts1.find({}, {random: 1, _id: 0}).sort({random: direction}).limit(limit).toArray();
    shardedAggTest.startBalancer();  // TODO: remove after fixing SERVER-9622
    var from_agg = aggregateOrdered(
        db.ts1, [{$project: {random: 1, _id: 0}}, {$sort: {random: direction}}, {$limit: limit}]);
    assert.eq(from_cursor, from_agg);
}
testSortLimit(1, 1);
testSortLimit(1, -1);
testSortLimit(10, 1);
testSortLimit(10, -1);
testSortLimit(100, 1);
testSortLimit(100, -1);

function testAvgStdDev() {
    jsTestLog('testing $avg and $stdDevPop in sharded $group');
    // Note: not using aggregateOrdered since it requires exact results. $stdDevPop can vary
    // slightly between runs if a migration occurs. This is why we use assert.close below.
    var res = db.ts1
                  .aggregate([{
                      $group: {
                          _id: null,
                          avg: {$avg: '$counter'},
                          stdDevPop: {$stdDevPop: '$counter'},
                      }
                  }])
                  .toArray();
    // http://en.wikipedia.org/wiki/Arithmetic_progression#Sum
    var avg = (1 + nItems) / 2;
    assert.close(res[0].avg, avg, '', 10 /*decimal places*/);

    // http://en.wikipedia.org/wiki/Arithmetic_progression#Standard_deviation
    var stdDev = Math.sqrt(((nItems - 1) * (nItems + 1)) / 12);
    assert.close(res[0].stdDevPop, stdDev, '', 10 /*decimal places*/);
}
testAvgStdDev();

function testSample() {
    jsTestLog('testing $sample');
    [0, 1, 10, nItems, nItems + 1].forEach(function(size) {
        var res = db.ts1.aggregate([{$sample: {size: size}}]).toArray();
        assert.eq(res.length, Math.min(nItems, size));
    });
}
testSample();

jsTestLog('test $out by copying source collection verbatim to output');
var outCollection = db.ts1_out;
var res = aggregateOrdered(db.ts1, [{$out: outCollection.getName()}]);
shardedAggTest.stopBalancer();  // TODO: remove after fixing SERVER-9622
assert.eq(db.ts1.find().itcount(), outCollection.find().itcount());
assert.eq(db.ts1.find().sort({_id: 1}).toArray(), outCollection.find().sort({_id: 1}).toArray());
shardedAggTest.startBalancer();  // TODO: remove after fixing SERVER-9622

// Make sure we error out if $out collection is sharded
assertErrorCode(outCollection, [{$out: db.ts1.getName()}], 17017);

assert.writeOK(db.literal.save({dollar: false}));

result = aggregateOrdered(
    db.literal,
    [{$project: {_id: 0, cost: {$cond: ['$dollar', {$literal: '$1.00'}, {$literal: '$.99'}]}}}]);

assert.eq([{cost: '$.99'}], result);

jsTestLog("Do a basic sharded explain. This just makes sure that it doesn't error and has " +
          "the right fields.");
var res = db.ts1.aggregate([{$project: {a: 1}}], {explain: true});
assert.commandWorked(res);
printjson(res);
assert("splitPipeline" in res);
assert("shardsPart" in res.splitPipeline);
assert("mergerPart" in res.splitPipeline);
assert("shards" in res);
for (var shardName in res.shards) {
    assert("host" in res.shards[shardName]);
    assert("stages" in res.shards[shardName]);
}

(function() {
    jsTestLog("Do a sharded explain from a mongod, not mongos, to ensure that it does not have " +
              "a SHARDING_FILTER stage.");
    var shardDb = shardedAggTest.shard0.getDB('aggShard');
    var res = shardDb.ts1.aggregate([{$match: {}}], {explain: true});
    printjson(res);
    assert.commandWorked(res);
    assert(!planHasStage(res.stages[0].$cursor.queryPlanner.winningPlan, "SHARDING_FILTER"));
}());

(function() {
    jsTestLog('Testing a $match stage on the shard key.');

    var outCollection = 'testShardKeyMatchOut';

    // Point query.
    var targetId = Math.floor(nItems * Math.random());
    var pipeline = [{$match: {_id: targetId}}, {$project: {_id: 1}}, {$sort: {_id: 1}}];
    var expectedDocs = [{_id: targetId}];
    // Normal pipeline.
    assert.eq(aggregateOrdered(db.ts1, pipeline), expectedDocs);
    // With $out.
    db[outCollection].drop();
    pipeline.push({$out: outCollection});
    db.ts1.aggregate(pipeline);
    assert.eq(db[outCollection].find().toArray(), expectedDocs);

    // Range query.
    var range = 500;
    var targetStart = Math.floor((nItems - range) * Math.random());
    pipeline = [
        {$match: {_id: {$gte: targetStart, $lt: targetStart + range}}},
        {$project: {_id: 1}},
        {$sort: {_id: 1}}
    ];
    expectedDocs = [];
    for (var i = targetStart; i < targetStart + range; i++) {
        expectedDocs.push({_id: i});
    }
    // Normal pipeline.
    assert.eq(aggregateOrdered(db.ts1, pipeline), expectedDocs);
    // With $out.
    db[outCollection].drop();
    pipeline.push({$out: outCollection});
    db.ts1.aggregate(pipeline);
    assert.eq(db[outCollection].find().toArray(), expectedDocs);
}());

// Call sub-tests designed to work sharded and unsharded.
// They check for this variable to know to shard their collections.
RUNNING_IN_SHARDED_AGG_TEST = true;  // global
jsTestLog('running jstests/aggregation/bugs/server9444.js');
load("jstests/aggregation/bugs/server9444.js");  // external sort
jsTestLog('running jstests/aggregation/bugs/server11675.js');
load("jstests/aggregation/bugs/server11675.js");  // text support

jsTestLog('shut everything down');
shardedAggTest.stop();
