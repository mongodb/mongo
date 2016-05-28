// server-3253 Unsharded support for $out
load('jstests/aggregation/extras/utils.js');

var input = db.server3253_in;
var inputDoesntExist = db.server3253_doesnt_exist;
var output = db.server3253_out;
var cappedOutput = db.server3253_out_capped;

input.drop();
inputDoesntExist.drop();  // never created
output.drop();

function collectionExists(coll) {
    return Array.contains(coll.getDB().getCollectionNames(), coll.getName());
}

function getOutputIndexes() {
    return output.getIndexes().sort(function(a, b) {
        if (a.name < b.name) {
            return -1;
        } else {
            return 1;
        }
    });
}

function test(input, pipeline, expected) {
    pipeline.push({$out: output.getName()});
    var indexes = getOutputIndexes();

    var cursor = input.aggregate(pipeline);

    assert.eq(cursor.itcount(), 0);                // empty cursor returned
    assert.eq(output.find().toArray(), expected);  // correct results
    var outputIndexes = getOutputIndexes();
    assert.eq(outputIndexes.length, indexes.length);  // number of indexes maintained
    for (var i = 0; i < outputIndexes.length; i++) {
        assert.docEq(outputIndexes[i], indexes[i]);
    }

    assert(collectionExists(output));
}

function listCollections(name) {
    var collectionInfosCursor = db.runCommand("listCollections", {filter: {name: name}});
    return new DBCommandCursor(db.getMongo(), collectionInfosCursor).toArray();
}

input.insert({_id: 1});
input.insert({_id: 2});
input.insert({_id: 3});

// insert into output so that the index exists and test() does not fail the first time around
output.insert({_id: 1});

// ensure there are no tmp agg_out collections before we begin
assert.eq([], listCollections(/tmp\.agg_out/));

// basic test
test(input,
     [{$project: {a: {$add: ['$_id', '$_id']}}}],
     [{_id: 1, a: 2}, {_id: 2, a: 4}, {_id: 3, a: 6}]);

// test with indexes
assert.eq(output.getIndexes().length, 1);
output.ensureIndex({a: 1});
assert.eq(output.getIndexes().length, 2);
test(input,
     [{$project: {a: {$multiply: ['$_id', '$_id']}}}],
     [{_id: 1, a: 1}, {_id: 2, a: 4}, {_id: 3, a: 9}]);

// test with empty result set and make sure old result is gone, but indexes remain
test(input, [{$match: {_id: 11}}], []);
assert.eq(output.getIndexes().length, 2);

// test with geo index
output.ensureIndex({b: "2d"});
assert.eq(output.getIndexes().length, 3);
test(input, [{$project: {b: "$_id"}}], [{_id: 1, b: 1}, {_id: 2, b: 2}, {_id: 3, b: 3}]);

// test with full text index
output.ensureIndex({c: "text"});
assert.eq(output.getIndexes().length, 4);
test(input, [{$project: {c: {$concat: ["hello there ", "_id"]}}}], [
    {_id: 1, c: "hello there _id"},
    {_id: 2, c: "hello there _id"},
    {_id: 3, c: "hello there _id"}
]);

// test with capped collection
cappedOutput.drop();
db.createCollection(cappedOutput.getName(), {capped: true, size: 2});
assertErrorCode(input, {$out: cappedOutput.getName()}, 17152);

// ensure everything works even if input doesn't exist.
test(inputDoesntExist, [], []);

// ensure we cant do dangerous things to system collections
var outputInSystem = db.system.server3253_out;
assertErrorCode(input, {$out: outputInSystem.getName()}, 17385);
assert(!collectionExists(outputInSystem));

// shoudn't leave temp collections laying around
assert.eq([], listCollections(/tmp\.agg_out/));
