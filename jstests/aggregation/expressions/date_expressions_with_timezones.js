/**
 * Test of date expressions with TZ.
 * Due to a change in format from int to long (SERVER-93063) this test is excluded for older version
 * from multiversion.
 *
 * @tags: [
 * requires_fcv_81,
 * ]
 */

import "jstests/libs/query/sbe_assert_error_override.js";

const coll = db.date_expressions_with_time_zones;
coll.drop();

assert.commandWorked(
    coll.insert([
        // Three sales on 2017-06-16 in UTC.
        {_id: 0, date: new ISODate("2017-06-16T00:00:00.000Z"), sales: 1},
        {_id: 1, date: new ISODate("2017-06-16T12:02:21.013Z"), sales: 2},
        // Six sales on 2017-06-17 in UTC.
        {_id: 2, date: new ISODate("2017-06-17T00:00:00.000Z"), sales: 2},
        {_id: 3, date: new ISODate("2017-06-17T12:02:21.013Z"), sales: 2},
        {_id: 4, date: new ISODate("2017-06-17T15:00:33.101Z"), sales: 2},
    ]),
);

// Compute how many sales happened on each day, in UTC.
assert.eq(
    [
        {_id: {year: 2017, month: 6, day: 16}, totalSales: 3},
        {_id: {year: 2017, month: 6, day: 17}, totalSales: 6},
    ],
    coll
        .aggregate([
            {
                $group: {
                    _id: {
                        year: {$year: "$date"},
                        month: {$month: "$date"},
                        day: {$dayOfMonth: "$date"},
                    },
                    totalSales: {$sum: "$sales"},
                },
            },
            {$sort: {"_id.year": 1, "_id.month": 1, "_id.day": 1}},
        ])
        .toArray(),
);

// Compute how many sales happened on each day, in New York. The sales made at midnight should
// move to the previous days.
assert.eq(
    [
        {_id: {year: 2017, month: 6, day: 15}, totalSales: 1},
        {_id: {year: 2017, month: 6, day: 16}, totalSales: 4},
        {_id: {year: 2017, month: 6, day: 17}, totalSales: 4},
    ],
    coll
        .aggregate([
            {
                $group: {
                    _id: {
                        year: {$year: {date: "$date", timezone: "America/New_York"}},
                        month: {$month: {date: "$date", timezone: "America/New_York"}},
                        day: {$dayOfMonth: {date: "$date", timezone: "America/New_York"}},
                    },
                    totalSales: {$sum: "$sales"},
                },
            },
            {$sort: {"_id.year": 1, "_id.month": 1, "_id.day": 1}},
        ])
        .toArray(),
);

// Compute how many sales happened on each day, in Sydney (+10 hours).
assert.eq(
    [
        {_id: {year: 2017, month: 6, day: 16}, totalSales: 3},
        {_id: {year: 2017, month: 6, day: 17}, totalSales: 4},
        {_id: {year: 2017, month: 6, day: 18}, totalSales: 2},
    ],
    coll
        .aggregate([
            {
                $group: {
                    _id: {
                        year: {$year: {date: "$date", timezone: "Australia/Sydney"}},
                        month: {$month: {date: "$date", timezone: "Australia/Sydney"}},
                        day: {$dayOfMonth: {date: "$date", timezone: "Australia/Sydney"}},
                    },
                    totalSales: {$sum: "$sales"},
                },
            },
            {$sort: {"_id.year": 1, "_id.month": 1, "_id.day": 1}},
        ])
        .toArray(),
);

assert(coll.drop());
assert.commandWorked(coll.insert({}));

function runDateTimeExpressionWithTimezone(exprName, tz) {
    let project = {};
    project[exprName] = tz ? {date: "$date", timezone: tz} : "$date";
    let pipeline = [{$project: {out: project}}];
    return coll.runCommand("aggregate", {pipeline: pipeline, cursor: {}});
}

function testDateTimeExpression(exprName, expectedValues) {
    assert(coll.drop());
    assert.commandWorked(coll.insert({date: ISODate("2017-01-16T01:02:03.456Z"), timezone: "America/Sao_Paulo"}));
    assert.eq(
        expectedValues.idBasedTzExpected,
        runDateTimeExpressionWithTimezone(exprName, "$timezone").cursor.firstBatch[0].out,
    );
    assert.eq(
        expectedValues.idBasedTzExpected,
        runDateTimeExpressionWithTimezone(exprName, "America/Sao_Paulo").cursor.firstBatch[0].out,
    );

    // Test expression with offset based timezone
    assert(coll.drop());
    assert.commandWorked(coll.insert({date: ISODate("2017-01-01T01:02:03.456Z"), timezone: "-01:30"}));
    assert.eq(
        expectedValues.offsetBasedTzExpected,
        runDateTimeExpressionWithTimezone(exprName, "$timezone").cursor.firstBatch[0].out,
    );
    assert.eq(
        expectedValues.offsetBasedTzExpected,
        runDateTimeExpressionWithTimezone(exprName, "-01:30").cursor.firstBatch[0].out,
    );

    // Test expression when document has no $timezone field
    assert(coll.drop());
    assert.commandWorked(coll.insert({date: ISODate("2017-01-16T01:02:03.456Z")}));
    assert.eq(null, runDateTimeExpressionWithTimezone(exprName, "$timezone").cursor.firstBatch[0].out);
    assert.eq(expectedValues.noTzExpected, runDateTimeExpressionWithTimezone(exprName).cursor.firstBatch[0].out);

    // Test expression when document has no date field
    assert(coll.drop());
    assert.commandWorked(coll.insert({timezone: "America/Sao_Paulo"}));
    assert.eq(null, runDateTimeExpressionWithTimezone(exprName, "$timezone").cursor.firstBatch[0].out);

    // test with invalid timezone identifier
    assert(coll.drop());
    assert.commandWorked(coll.insert({date: ISODate("2017-06-16T00:00:00.000Z"), timezone: "USA"}));
    assert.commandFailedWithCode(runDateTimeExpressionWithTimezone(exprName, "$timezone"), 40485);
    assert.commandFailedWithCode(runDateTimeExpressionWithTimezone(exprName, "USA"), 40485);

    // test with invalid timezone type
    assert(coll.drop());
    assert.commandWorked(coll.insert({date: ISODate("2017-06-16T00:00:00.000Z"), timezone: 123}));
    assert.commandFailedWithCode(runDateTimeExpressionWithTimezone(exprName, "$timezone"), 40533);
    assert.commandFailedWithCode(runDateTimeExpressionWithTimezone(exprName, 1111), 40533);

    // test with invalid date type
    assert(coll.drop());
    assert.commandWorked(coll.insert({date: "2017-06-16T00:00:00.000Z", timezone: "America/Sao_Paulo"}));
    assert.commandFailedWithCode(runDateTimeExpressionWithTimezone(exprName, "$timezone"), 16006);
}

testDateTimeExpression("$dayOfWeek", {idBasedTzExpected: 1, offsetBasedTzExpected: 7, noTzExpected: 2});
testDateTimeExpression("$dayOfMonth", {idBasedTzExpected: 15, offsetBasedTzExpected: 31, noTzExpected: 16});
testDateTimeExpression("$dayOfYear", {idBasedTzExpected: 15, offsetBasedTzExpected: 366, noTzExpected: 16});
testDateTimeExpression("$year", {idBasedTzExpected: 2017, offsetBasedTzExpected: 2016, noTzExpected: 2017});
testDateTimeExpression("$month", {idBasedTzExpected: 1, offsetBasedTzExpected: 12, noTzExpected: 1});
testDateTimeExpression("$hour", {idBasedTzExpected: 23, offsetBasedTzExpected: 23, noTzExpected: 1});
testDateTimeExpression("$minute", {idBasedTzExpected: 2, offsetBasedTzExpected: 32, noTzExpected: 2});
testDateTimeExpression("$second", {idBasedTzExpected: 3, offsetBasedTzExpected: 3, noTzExpected: 3});
testDateTimeExpression("$millisecond", {idBasedTzExpected: 456, offsetBasedTzExpected: 456, noTzExpected: 456});
testDateTimeExpression("$week", {idBasedTzExpected: 3, offsetBasedTzExpected: 52, noTzExpected: 3});
testDateTimeExpression("$isoWeekYear", {idBasedTzExpected: 2017, offsetBasedTzExpected: 2016, noTzExpected: 2017});
testDateTimeExpression("$isoDayOfWeek", {idBasedTzExpected: 7, offsetBasedTzExpected: 6, noTzExpected: 1});
testDateTimeExpression("$isoWeek", {idBasedTzExpected: 2, offsetBasedTzExpected: 52, noTzExpected: 3});

// Make sure the data type returned by the date/time expressions is correct
function testDateTimeExpressionType(exprName, exprType) {
    let expr = {};
    let type = {};
    type["$type"] = exprType;
    let pipeline = [{$addFields: {"tp": expr}}, {$match: {"tp": type}}, {$project: {"_id": 1}}];

    // without timezone
    expr[exprName] = "$date";
    assert.eq([{_id: 0}], coll.aggregate(pipeline).toArray());

    // with timezone
    expr[exprName] = {date: "$date", timezone: "America/Sao_Paulo"};
    assert.eq([{_id: 0}], coll.aggregate(pipeline).toArray());
}
assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, date: ISODate("2017-01-16T01:02:03.456Z")}));
testDateTimeExpressionType("$dayOfWeek", "int");
testDateTimeExpressionType("$dayOfMonth", "int");
testDateTimeExpressionType("$dayOfYear", "int");
testDateTimeExpressionType("$year", "int");
testDateTimeExpressionType("$month", "int");
testDateTimeExpressionType("$hour", "int");
testDateTimeExpressionType("$minute", "int");
testDateTimeExpressionType("$second", "int");
testDateTimeExpressionType("$millisecond", "int");
testDateTimeExpressionType("$week", "int");
testDateTimeExpressionType("$isoWeekYear", "long");
testDateTimeExpressionType("$isoDayOfWeek", "int");
testDateTimeExpressionType("$isoWeek", "int");
