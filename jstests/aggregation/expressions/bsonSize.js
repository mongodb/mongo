const coll = db.agg_expr_bsonSize;
coll.drop();
assert.commandWorked(coll.insert({_id: 1}));

function checkBsonSize() {
    assert.eq(Object.bsonsize(coll.findOne()), coll.aggregate([{$project: {s: {$bsonSize: "$$CURRENT"}}}]).next().s);
}

checkBsonSize();

assert.commandWorked(coll.update({_id: 1}, {$push: {xs: 1}}));
checkBsonSize();

assert.commandWorked(coll.update({_id: 1}, {$push: {xs: {subdoc: 12345}}}));
checkBsonSize();

assert.commandWorked(coll.update({_id: 1}, {$push: {xs: "x".repeat(7)}}));
checkBsonSize();

assert.commandWorked(coll.update({_id: 1}, {$push: {xs: "x".repeat(500)}}));
checkBsonSize();

assert.commandWorked(coll.update({_id: 1}, {$push: {xs: "x".repeat(16 * 1e6)}}));
checkBsonSize();

// embedded arrays
assert.commandWorked(coll.update({_id: 1}, {$set: {arr: [1, 2, 3, 4]}}));
checkBsonSize();

// subdocuments
assert.commandWorked(coll.update({_id: 1}, {$set: {arr: {a: {b: {c: 1}}}}}));
checkBsonSize();

// bsonSize's argument must be a document
function checkExpectsDocument(badInput) {
    assert.throws(
        () => coll.aggregate([{$project: {x: {$bsonSize: {$literal: badInput}}}}]),
        [],
        "$bsonSize requires a document input",
    );
}
checkExpectsDocument(123);
checkExpectsDocument("abc");
checkExpectsDocument(BinData(0, "aaaa"));
checkExpectsDocument([123, 456]);
checkExpectsDocument([{x: 1}, {y: 2}]);

// SERVER-79019: $bsonSize must work on intermediate pipeline results larger than 16MiB.
const isMultiversion =
    Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion);
const is90OrHigher = MongoRunner.compareBinVersions(lastLTSFCV, "9.0") >= 0;
if (!isMultiversion || is90OrHigher) {
    const largeColl = db.agg_expr_bsonSize_large;
    largeColl.drop();

    // Insert a document with a ~10MB string, which is within the 16MB BSON document limit.
    const largeString = "x".repeat(10 * 1024 * 1024);
    assert.commandWorked(largeColl.insertOne({_id: 1, a: largeString}));

    // Use $addFields to produce an intermediate document with two ~10MiB string fields,
    // making the total document size exceed 16MB. Then apply $bsonSize to $$ROOT.
    // The $project output is a small document with just the computed size value.
    const result = largeColl
        .aggregate([{$addFields: {b: {$concat: ["$a", "-"]}}}, {$project: {_id: 0, size: {$bsonSize: "$$ROOT"}}}])
        .toArray();

    assert.eq(1, result.length);
    // The intermediate document has two ~10MB fields so its BSON size exceeds 16MB.
    assert.gt(result[0].size, 16 * 1024 * 1024, "Expected $bsonSize to return a value > 16MB");

    // Also verify the use case from the ticket: using $bsonSize in $match to filter by size.
    const matchResult = largeColl
        .aggregate([
            {$addFields: {b: {$concat: ["$a", "-"]}}},
            {$match: {$expr: {$gt: [{$bsonSize: "$$ROOT"}, 16 * 1024 * 1024]}}},
            {$count: "matching"},
        ])
        .toArray();
    assert.eq(1, matchResult.length, "Expected one document whose intermediate size exceeds 16MB");
    assert.eq(1, matchResult[0].matching);

    largeColl.drop();
}
