// SERVER-11118 Tests for $dateToString
import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];

// Used to verify expected output format
function testFormat(date, formatStr, expectedStr) {
    coll.drop();
    assert.commandWorked(coll.insert({date: date}));

    const res = coll
        .aggregate([{$project: {_id: 0, formatted: {$dateToString: {format: formatStr, date: "$date"}}}}])
        .toArray();

    assert.eq(res[0].formatted, expectedStr);
}

// Used to verify that server recognizes bad formats
function testFormatError(formatObj, errCode) {
    coll.drop();
    assert.commandWorked(coll.insert({date: ISODate()}));

    assertErrorCode(coll, {$project: {_id: 0, formatted: {$dateToString: formatObj}}}, errCode);
}

// Used to verify that only date values are accepted for date parameter
function testDateValueError(dateVal, errCode) {
    coll.drop();
    assert.commandWorked(coll.insert({date: dateVal}));

    assertErrorCode(coll, {$project: {formatted: {$dateToString: {format: "%Y", date: "$date"}}}}, errCode);
}

const now = ISODate();

// Use all modifiers we can test with js provided function
testFormat(
    now,
    "%%-%Y-%m-%d-%H-%M-%S-%L",
    [
        "%",
        now.getUTCFullYear().zeroPad(4),
        (now.getUTCMonth() + 1).zeroPad(2),
        now.getUTCDate().zeroPad(2),
        now.getUTCHours().zeroPad(2),
        now.getUTCMinutes().zeroPad(2),
        now.getUTCSeconds().zeroPad(2),
        now.getUTCMilliseconds().zeroPad(3),
    ].join("-"),
);

// Padding tests
const padme = ISODate("2001-02-03T04:05:06.007Z");

testFormat(padme, "%%", "%");
testFormat(padme, "%Y", padme.getUTCFullYear().zeroPad(4));
testFormat(padme, "%m", (padme.getUTCMonth() + 1).zeroPad(2));
testFormat(padme, "%d", padme.getUTCDate().zeroPad(2));
testFormat(padme, "%H", padme.getUTCHours().zeroPad(2));
testFormat(padme, "%M", padme.getUTCMinutes().zeroPad(2));
testFormat(padme, "%S", padme.getUTCSeconds().zeroPad(2));
testFormat(padme, "%L", padme.getUTCMilliseconds().zeroPad(3));

// no space and multiple characters between modifiers
testFormat(
    now,
    "%d%d***%d***%d**%d*%d",
    [
        now.getUTCDate().zeroPad(2),
        now.getUTCDate().zeroPad(2),
        "***",
        now.getUTCDate().zeroPad(2),
        "***",
        now.getUTCDate().zeroPad(2),
        "**",
        now.getUTCDate().zeroPad(2),
        "*",
        now.getUTCDate().zeroPad(2),
    ].join(""),
);

// JS doesn't have equivalents of these format specifiers
testFormat(ISODate("1999-01-02 03:04:05.006Z"), "%U-%w-%j", "00-7-002");

// Missing date
testFormatError({format: "%Y"}, 18628);

// Extra field
testFormatError({format: "%Y", date: "$date", extra: "whyamIhere"}, 18534);

// Not an object
testFormatError(["%Y", "$date"], 18629);

// Use invalid modifier at middle of string
testFormatError({format: "%Y-%q", date: "$date"}, 18536);

// Odd number of percent signs at end
testFormatError({format: "%U-%w-%j-%%%", date: "$date"}, 18535);

// Odd number of percent signs at middle
// will get interpreted as an invalid modifier since it will try to use '%A'
testFormatError({format: "AAAAA%%%AAAAAA", date: "$date"}, 18536);

// Format parameter not a string
testFormatError({format: {iamalion: "roar"}, date: "$date"}, 18533);

///
/// Additional Tests
///

// Test document
const date = ISODate("1999-08-29");

testFormat(date, "%%d", "%d");

// A very long string of "%"s
const longstr = "%%".repeat(999);
const halfstr = "%".repeat(999);
testFormat(date, longstr, halfstr);

// Dates as null (should return a null)
testFormat(null, "%Y", null);

///
/// Using non-date fields as date parameter *should fail*
///

// Array
testDateValueError([], 16006);
testDateValueError([1, 2, 3], 16006);

// Sub-object
testDateValueError({}, 16006);
testDateValueError({a: 1}, 16006);

// String
testDateValueError("blahblahblah", 16006);

// Integer
testDateValueError(1234, 16006);

///
/// Using non-string fields as format strings
///

// Array
testFormatError({format: [], date: "$date"}, 18533);
testFormatError({format: [1, 2, 3], date: "$date"}, 18533);

// Integer
testFormatError({format: 1, date: "$date"}, 18533);

// Date
testFormatError({format: ISODate(), date: "$date"}, 18533);
