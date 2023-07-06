import "jstests/libs/sbe_assert_error_override.js";

var testDB = db.getSiblingDB("expression_ts_second_increment");

assert.commandWorked(testDB.dropDatabase());

var coll = testDB.getCollection("test");

assert.commandWorked(
    coll.insert({_id: 0, bsonTime: Timestamp(1622731060, 10), invalidBsonTime: 1622731060}));

(function testtsSecond() {
    // Projects the seconds component of the bson timestamp using the 'tsSecond' on the 'bsonTime'
    // field and verifies that the value is correct.
    let result =
        coll.aggregate([{$project: {bsonTime: 1, bsonTimeSeconds: {$tsSecond: "$bsonTime"}}}])
            .toArray();
    assert.eq(result.length, 1);
    assert.eq(result[0].bsonTimeSeconds, result[0].bsonTime.getTime(), result);

    // Passes a non existing field path to the '$tsSecond' and verifies a null timestamp is
    // returned.
    result = coll.aggregate(
                     [{$project: {bsonTime: 1, bsonTimeSeconds: {$tsSecond: "$nonExistingField"}}}])
                 .toArray();
    assert.eq(result.length, 1);
    assert.eq(result[0].bsonTimeSeconds, null, result);

    // Projects the seconds component of bson timestamp using the 'tsSecond' on the
    // 'invalidBsonTime' field and verifies that an expected error code is thrown.
    const nonTimestampError = assert.throws(
        () => coll.aggregate(
                      [{$project: {bsonTime: 1, bsonTimeSeconds: {$tsSecond: "$invalidBsonTime"}}}])
                  .toArray());
    assert.commandFailedWithCode(nonTimestampError, 5687301);
})();

(function testtsIncrement() {
    // Projects the increment component of bson timestamp using the 'tsSecond' on the 'bsonTime'
    // field and verifies that the value is correct.
    let result =
        coll.aggregate([{$project: {bsonTime: 1, bsonTimeIncrements: {$tsIncrement: "$bsonTime"}}}])
            .toArray();
    assert.eq(result.length, 1);
    assert.eq(result[0].bsonTimeIncrements, result[0].bsonTime.getInc(), result);

    // Passes a non existing field path to the '$tsIncrement' and verifies a null timestamp is
    // returned.
    result =
        coll.aggregate(
                [{$project: {bsonTime: 1, bsonTimeSeconds: {$tsIncrement: "$nonExistingField"}}}])
            .toArray();
    assert.eq(result.length, 1);
    assert.eq(result[0].bsonTimeSeconds, null, result);

    // Projects the increment component of bson timestamp using the 'tsIncrement' on the
    // 'invalidBsonTime' field and verifies that an expected error code is thrown.
    const nonTimestampError = assert.throws(
        () =>
            coll.aggregate([{
                    $project: {bsonTime: 1, bsonTimeIncrements: {$tsIncrement: "$invalidBsonTime"}}
                }])
                .toArray());
    assert.commandFailedWithCode(nonTimestampError, 5687302);
})();
