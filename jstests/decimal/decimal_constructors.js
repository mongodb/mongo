// Tests constructing NumberDecimal with various types

(function() {
'use strict';
var col = db.d_constructors;
col.drop();

// Insert some sample data.

assert.commandWorked(col.insert([
    {d: NumberDecimal('1')},
    {d: NumberDecimal(1)},
    {d: NumberDecimal(NumberLong('1'))},
    {d: NumberDecimal(NumberInt('1'))},
    {d: NumberDecimal('NaN')},
    {d: NumberDecimal('-NaN')}
]),
                     'Initial insertion of decimals failed');

var exactDoubleString = "1427247692705959881058285969449495136382746624";
var exactDoubleTinyString =
    "0.00000000000000000000000000000000000000000000000000000000000062230152778611417071440640537801242405902521687211671331011166147896988340353834411839448231257136169569665895551224821247160434722900390625";

assert.throws(
    NumberDecimal, [exactDoubleString], 'Unexpected success in creating invalid Decimal128');
assert.throws(
    NumberDecimal, [exactDoubleTinyString], 'Unexpected success in creating invalid Decimal128');
assert.throws(NumberDecimal, ['some garbage'], 'Unexpected success in creating invalid Decimal128');

// Find values with various types and NumberDecimal constructed types
assert.eq(col.find({'d': NumberDecimal('1')}).count(), '4');
assert.eq(col.find({'d': NumberDecimal(1)}).count(), '4');
assert.eq(col.find({'d': NumberDecimal(NumberLong(1))}).count(), '4');
assert.eq(col.find({'d': NumberDecimal(NumberInt(1))}).count(), '4');
assert.eq(col.find({'d': 1}).count(), '4');
assert.eq(col.find({'d': NumberLong(1)}).count(), '4');
assert.eq(col.find({'d': NumberInt(1)}).count(), '4');
// NaN and -NaN are both evaluated to NaN
assert.eq(col.find({'d': NumberDecimal('NaN')}).count(), 2);

// Verify that shell 'assert.eq' considers precision during comparison.
assert.neq(NumberDecimal('1'), NumberDecimal('1.000'));
assert.neq(NumberDecimal('0'), NumberDecimal('-0'));

// Verify the behavior of 'numberDecimalsEqual' helper.
assert(numberDecimalsEqual(NumberDecimal('10.20'), NumberDecimal('10.2')));
assert.throws(
    () => numberDecimalsEqual(NumberDecimal('10.20'), NumberDecimal('10.2'), "Third parameter"));
assert.throws(() => numberDecimalsEqual(NumberDecimal('10.20'), "Wrong parameter type"));

// Verify the behavior of 'numberDecimalsAlmostEqual' helper.
assert(numberDecimalsAlmostEqual(NumberDecimal("10001"), NumberDecimal("10002"), 3));
assert(!numberDecimalsAlmostEqual(NumberDecimal("10001"), NumberDecimal("10002"), 5));

// Regression tests for BF-24149.
assert(numberDecimalsAlmostEqual(NumberDecimal("905721242210.0455427920454969568"),
                                 NumberDecimal("905721242210.0453137831269007622941"),
                                 15));

assert(!numberDecimalsAlmostEqual(NumberDecimal("905721242210.0455427920454969568"),
                                  NumberDecimal("905721242210.0453137831269007622941"),
                                  16));

// Verify helper works the same with negative numers.
assert(numberDecimalsAlmostEqual(NumberDecimal("-10001"), NumberDecimal("-10002"), 3));
assert(!numberDecimalsAlmostEqual(NumberDecimal("-10001"), NumberDecimal("-10002"), 5));
assert(numberDecimalsAlmostEqual(NumberDecimal("-905721242210.0455427920454969568"),
                                 NumberDecimal("-905721242210.0453137831269007622941"),
                                 15));
assert(!numberDecimalsAlmostEqual(NumberDecimal("-905721242210.0455427920454969568"),
                                  NumberDecimal("-905721242210.0453137831269007622941"),
                                  16));

// Verify mixed-sign arguments aren't equal.
assert(!numberDecimalsAlmostEqual(NumberDecimal("10001"), NumberDecimal("-10002"), 3));
assert(!numberDecimalsAlmostEqual(NumberDecimal("-10001"), NumberDecimal("10002"), 3));
assert(!numberDecimalsAlmostEqual(NumberDecimal("-905721242210.0455427920454969568"),
                                  NumberDecimal("905721242210.0453137831269007622941"),
                                  10));
assert(!numberDecimalsAlmostEqual(NumberDecimal("905721242210.0455427920454969568"),
                                  NumberDecimal("-905721242210.0453137831269007622941"),
                                  10));

// Regression test for BF-25335
assert(numberDecimalsAlmostEqual(NumberDecimal("-104735446372966662851.0135276410897"),
                                 NumberDecimal("-104735446372966750508.0426966190306"),
                                 10));
// Regression test for BF-25390
assert(numberDecimalsAlmostEqual(NumberDecimal("-331.0000000000"), NumberDecimal("-331"), 10));

// Verify numberDecimalsAlmostEqual() works with 0.
assert(numberDecimalsAlmostEqual(NumberDecimal("0"), NumberDecimal("0"), 10));
assert(numberDecimalsAlmostEqual(NumberDecimal("0"), NumberDecimal("0.0000000000000001"), 10));
assert(numberDecimalsAlmostEqual(NumberDecimal("0.0000000000000001"), NumberDecimal("0"), 10));
assert(!numberDecimalsAlmostEqual(NumberDecimal("1.00000000000000"), NumberDecimal("1.1"), 10));
assert(numberDecimalsAlmostEqual(
    NumberDecimal("1.00000000000000"), NumberDecimal("1.00000000000001"), 10));
assert(numberDecimalsAlmostEqual(NumberDecimal("Infinity"), NumberDecimal("Infinity"), 10));
assert(!numberDecimalsAlmostEqual(NumberDecimal("Infinity"), NumberDecimal("-Infinity"), 10));
assert(!numberDecimalsAlmostEqual(NumberDecimal("Infinity"), NumberDecimal("NaN"), 10));
assert(numberDecimalsAlmostEqual(NumberDecimal("NaN"), NumberDecimal("NaN"), 10));
assert(!numberDecimalsAlmostEqual(NumberDecimal("0"), NumberDecimal("0.000001"), 10));
}());
