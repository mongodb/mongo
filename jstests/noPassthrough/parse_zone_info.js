// Tests the parsing of the timeZoneInfo parameter and file use.
(function() {
// Test that a bad file causes startup to fail.
let conn = MongoRunner.runMongod({timeZoneInfo: "jstests/libs/config_files/bad_timezone_info"});
assert.eq(conn, null, "expected launching mongod with bad timezone rules to fail");
assert.neq(-1, rawMongoProgramOutput().search(/Fatal assertion.*40475/));

// Test that a non-existent directory causes startup to fail.
conn = MongoRunner.runMongod({timeZoneInfo: "jstests/libs/config_files/missing_directory"});
assert.eq(conn, null, "expected launching mongod with bad timezone rules to fail");

// Look for either old or new error message
assert(rawMongoProgramOutput().includes("Error creating service context") ||
       rawMongoProgramOutput().includes("Failed to create service context"));

function testWithGoodTimeZoneDir(tz_good_path) {
    conn = MongoRunner.runMongod({timeZoneInfo: tz_good_path});
    assert.neq(conn, null, "expected launching mongod with good timezone rules to succeed");

    // Test that can use file-provided timezones in an expression.
    const testDB = conn.getDB("test");
    const coll = testDB.parse_zone_info;
    assert.commandWorked(coll.insert({x: new Date()}));
    assert.doesNotThrow(
        () => coll.aggregate([{$set: {x_parts: {$dateToParts: {date: "$x", timezone: "GMT"}}}}]));
    assert.doesNotThrow(
        () => coll.aggregate(
            [{$set: {x_parts: {$dateToParts: {date: "$x", timezone: "America/Sao_Paulo"}}}}]));

    // Confirm that attempt to use a non-existent timezone in an expression fails.
    const err =
        assert.throws(() => coll.aggregate(
                          [{$set: {x_parts: {$dateToParts: {date: "$x", timezone: "Unknown"}}}}]));
    assert.eq(err.code, 40485);

    // Test some dates which specifically exercise slim-format timezone files. The test-case below
    // produced incorrect output using `$dateToParts` on `timelib-2018.01` with slim timezone format
    // and the "America/New York" timezone, since timelib was unable to correctly extrapolate DST
    // changes from the slim-format files. This was fixed in the timelib 2021 series.
    const corner_coll = testDB.parse_zone_info_corner_cases;

    test_dates = [
        {
            test_date: "2020-10-20T19:49:47.634Z",
            test_date_parts: {
                "year": 2020,
                "month": 10,
                "day": 20,
                "hour": 15,
                "minute": 49,
                "second": 47,
                "millisecond": 634
            }
        },
        {
            test_date: "2020-12-14T12:00:00Z",
            test_date_parts: {
                "year": 2020,
                "month": 12,
                "day": 14,
                "hour": 7,
                "minute": 0,
                "second": 0,
                "millisecond": 0
            }
        }
    ];

    for (let i = 0; i < test_dates.length; ++i) {
        assert.commandWorked(corner_coll.insert({_id: i, x: new ISODate(test_dates[i].test_date)}));
    }

    let res = corner_coll
                  .aggregate([
                      {$set: {x_parts: {$dateToParts: {date: "$x", timezone: "America/New_York"}}}},
                      {$sort: {_id: 1}}
                  ])
                  .toArray();
    for (let i = 0; i < test_dates.length; ++i) {
        assert.eq(res[i].x_parts, test_dates[i].test_date_parts);
    }

    MongoRunner.stopMongod(conn);
}

// Test that startup can succeed with a good file.
testWithGoodTimeZoneDir("jstests/libs/config_files/good_timezone_info_fat");
testWithGoodTimeZoneDir("jstests/libs/config_files/good_timezone_info_slim");
}());
