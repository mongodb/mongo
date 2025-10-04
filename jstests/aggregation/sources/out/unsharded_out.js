// Cannot implicitly shard accessed collections because unsupported use of sharded collection
// for output collection of aggregation pipeline.
// @tags: [
//   assumes_superuser_permissions,
//   assumes_unsharded_collection,
// ]

// server-3253 Unsharded support for $out
import {anyEq, assertErrorCode, collectionExists} from "jstests/aggregation/extras/utils.js";

const testDb = db.getSiblingDB("unsharded_out");
let input = testDb.unsharded_out_in;
let inputDoesntExist = testDb.unsharded_out_doesnt_exist;
let output = testDb.unsharded_out_out;
let cappedOutput = testDb.unsharded_out_out_capped;

input.drop();
inputDoesntExist.drop(); // never created
output.drop();

function getOutputIndexes() {
    return output.getIndexes().sort(function (a, b) {
        if (a.name < b.name) {
            return -1;
        } else {
            return 1;
        }
    });
}

function test(input, pipeline, expected) {
    pipeline.push({$out: output.getName()});
    let indexes = getOutputIndexes();

    let cursor = input.aggregate(pipeline);

    assert.eq(cursor.itcount(), 0); // empty cursor returned
    assert(anyEq(output.find().toArray(), expected)); // correct results
    let outputIndexes = getOutputIndexes();
    assert.eq(outputIndexes.length, indexes.length); // number of indexes maintained
    for (let i = 0; i < outputIndexes.length; i++) {
        assert.docEq(outputIndexes[i], indexes[i]);
    }

    assert(collectionExists(output));
}

function listCollections(name) {
    let collectionInfosCursor = testDb.runCommand("listCollections", {filter: {name: name}});
    return new DBCommandCursor(testDb, collectionInfosCursor).toArray();
}

input.insert({_id: 1});
input.insert({_id: 2});
input.insert({_id: 3});

// insert into output so that the index exists and test() does not fail the first time around
output.insert({_id: 1});

// ensure there are no tmp agg_out collections before we begin
assert.eq([], listCollections(/tmp\.agg_out/));

// basic test
test(
    input,
    [{$project: {a: {$add: ["$_id", "$_id"]}}}],
    [
        {_id: 1, a: 2},
        {_id: 2, a: 4},
        {_id: 3, a: 6},
    ],
);

// test with indexes
assert.eq(output.getIndexes().length, 1);
output.createIndex({a: 1});
assert.eq(output.getIndexes().length, 2);
test(
    input,
    [{$project: {a: {$multiply: ["$_id", "$_id"]}}}],
    [
        {_id: 1, a: 1},
        {_id: 2, a: 4},
        {_id: 3, a: 9},
    ],
);

// test with empty result set and make sure old result is gone, but indexes remain
test(input, [{$match: {_id: 11}}], []);
assert.eq(output.getIndexes().length, 2);

// test with geo index
output.createIndex({b: "2d"});
assert.eq(output.getIndexes().length, 3);
test(
    input,
    [{$project: {b: "$_id"}}],
    [
        {_id: 1, b: 1},
        {_id: 2, b: 2},
        {_id: 3, b: 3},
    ],
);

// test with full text index
output.createIndex({c: "text"});
assert.eq(output.getIndexes().length, 4);
test(
    input,
    [{$project: {c: {$concat: ["hello there ", "_id"]}}}],
    [
        {_id: 1, c: "hello there _id"},
        {_id: 2, c: "hello there _id"},
        {_id: 3, c: "hello there _id"},
    ],
);

cappedOutput.drop();
testDb.createCollection(cappedOutput.getName(), {capped: true, size: 2});
assertErrorCode(input, {$out: cappedOutput.getName()}, 17152);

// ensure everything works even if input doesn't exist.
test(inputDoesntExist, [], []);

// shoudn't leave temp collections laying around
assert.eq([], listCollections(/tmp\.agg_out/));
