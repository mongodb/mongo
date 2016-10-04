// Tests constructing NumberDecimal with various types

(function() {
    'use strict';
    var col = db.d_constructors;
    col.drop();

    // Insert some sample data.

    assert.writeOK(col.insert([
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
    assert.throws(NumberDecimal,
                  [exactDoubleTinyString],
                  'Unexpected success in creating invalid Decimal128');
    assert.throws(
        NumberDecimal, ['some garbage'], 'Unexpected success in creating invalid Decimal128');

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
}());
