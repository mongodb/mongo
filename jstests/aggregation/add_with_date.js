(function() {
"use strict";

const coll = db.getSiblingDB(jsTestName()).coll;
coll.drop();

function getResultOfExpression(expr) {
    const resultArray = coll.aggregate({$project: {computed: expr}}).toArray();
    assert.eq(1, resultArray.length);
    return resultArray[0].computed;
}

assert.commandWorked(coll.insert({
    _id: 0,
    decimalVal: NumberDecimal("819.5359123621083"),
    doubleVal: 819.536,
    int64Val: NumberLong(820),
    int32Val: NumberInt(820),
    dateVal: ISODate("2019-01-30T07:30:10.137Z"),
    overflowDecimal: NumberDecimal("1e5000"),
    overflowDouble: 1e1000,
    overflowInt64: NumberLong("9223372036854775807"),
    nanDouble: NaN,
    nanDecimal: NumberDecimal("NaN"),
}));

// Adding a Decimal128 value to a date literal.
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: ["$decimalVal", ISODate("2019-01-30T07:30:10.137Z")]}));
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: [ISODate("2019-01-30T07:30:10.137Z"), "$decimalVal"]}));

// Adding a Decimal128 literal to a date value.
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: ["$dateVal", NumberDecimal("819.5359123621083")]}));
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: [NumberDecimal("819.5359123621083"), "$dateVal"]}));

// Adding a Decimal128 value to a date value.
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: ["$dateVal", "$decimalVal"]}));
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: ["$decimalVal", "$dateVal"]}));

// Adding a double value to a date value.
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: ["$dateVal", "$doubleVal"]}));
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: ["$doubleVal", "$dateVal"]}));

// Adding an int64_t value to date value.
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: ["$dateVal", "$int64Val"]}));
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: ["$int64Val", "$dateVal"]}));

// Adding an int32_t value to date value.
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: ["$dateVal", "$int32Val"]}));
assert.eq(ISODate("2019-01-30T07:30:10.957Z"),
          getResultOfExpression({$add: ["$int32Val", "$dateVal"]}));

// Addition with a date and multiple values of differing data types.
assert.eq(ISODate("2019-01-30T07:30:12.596Z"),
          getResultOfExpression({$add: ["$dateVal", "$decimalVal", "$doubleVal", "$int64Val"]}));
assert.eq(ISODate("2019-01-30T07:30:12.596Z"),
          getResultOfExpression({$add: ["$decimalVal", "$dateVal", "$doubleVal", "$int64Val"]}));
assert.eq(ISODate("2019-01-30T07:30:12.596Z"),
          getResultOfExpression({$add: ["$decimalVal", "$doubleVal", "$int64Val", "$dateVal"]}));

// The result of an addition must remain in the range of int64_t in order to convert back to a Date;
// an overflow into the domain of double-precision floating point numbers triggers a query-fatal
// error.
let err = assert.throws(() => getResultOfExpression({$add: ["$dateVal", "$overflowDouble"]}));
assert.eq(ErrorCodes.Overflow, err.code);

err = assert.throws(() => getResultOfExpression({$add: ["$dateVal", "$overflowInt64"]}));
assert.eq(ErrorCodes.Overflow, err.code);

err = assert.throws(
    () => getResultOfExpression({$add: ["$dateVal", "$int64Val", "$overflowDouble"]}));
assert.eq(ErrorCodes.Overflow, err.code);

err = assert.throws(
    () => getResultOfExpression({$add: ["$int64Val", "$dateVal", "$overflowDouble"]}));
assert.eq(ErrorCodes.Overflow, err.code);

// One quirk of date addition semantics is that an overflow into the domain of Decimal128 is not
// fatal and instead results in an invalid "NaN" Date value.
const nanDate = new Date("");
assert.eq(nanDate, getResultOfExpression({$add: ["$dateVal", "$overflowDecimal"]}));
assert.eq(nanDate,
          getResultOfExpression({$add: ["$dateVal", "$overflowDouble", "$overflowDecimal"]}));
assert.eq(nanDate, getResultOfExpression({$add: ["$int64Val", "$dateVal", "$overflowDecimal"]}));

// Adding a double-typed NaN to a date value.
err = assert.throws(() => getResultOfExpression({$add: ["$dateVal", "$nanDouble"]}));
assert.eq(ErrorCodes.Overflow, err.code);

err = assert.throws(() => getResultOfExpression({$add: ["$nanDouble", "$dateVal"]}));
assert.eq(ErrorCodes.Overflow, err.code);

// Adding a Decimal128-typed NaN to a date value.
assert.eq(nanDate, getResultOfExpression({$add: ["$dateVal", "$nanDecimal"]}));
assert.eq(nanDate, getResultOfExpression({$add: ["$nanDecimal", "$dateVal"]}));

// Addition with a date, a double-typed NaN, and a third value.
err = assert.throws(() => getResultOfExpression({$add: ["$dateVal", "$doubleVal", "$nanDouble"]}));
assert.eq(ErrorCodes.Overflow, err.code);

// Addition with a date, and both types of NaN.
assert.eq(nanDate, getResultOfExpression({$add: ["$dateVal", "$nanDouble", "$nanDecimal"]}));
}());
