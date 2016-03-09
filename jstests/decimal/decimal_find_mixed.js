// Tests finding NumberDecimal from the shell in mixed collections.

(function() {
    "use strict";
    var col = db.decimal_find_mixed;
    col.drop();

    // Insert some sample data.

    assert.writeOK(col.insert([
        {"a": -1},
        {"a": NumberDecimal("-1")},
        {"a": NumberLong("-1")},
        {"a": NumberInt("-1")},
        {"a": -0.3},
        {"a": NumberDecimal("-0.3")},
        {"a": -0.1},
        {"a": NumberDecimal("-0.1")},
        {"a": NumberDecimal("0")},
        {"a": 0},
        {"a": NumberDecimal("-0")},
        {"a": NumberDecimal("0.00")},
        {"a": NumberDecimal("0.1")},
        {"a": 0.1},
        {"a": NumberDecimal("0.3")},
        {"a": 0.3},
        {"a": NumberDecimal("0.5")},
        {"a": 0.5},
        {"a": NumberDecimal("1.0")},
        {"a": NumberLong("1")},
        {"a": NumberDecimal("1.00")},
        {"a": NumberDecimal("2.00")},
        {"a": NumberDecimal("12345678901234567890.12345678901234")},
        {"a": NumberDecimal("NaN")},
        {"a": NumberDecimal("-NaN")},
        {"a": NaN},
        {"a": NumberDecimal("Infinity")},
        {"a": Infinity}
    ]),
                   "Initial decimal insertion failed");

    // Simple finds
    assert.eq(col.find({"a": -1}).count(), 4, "A1");
    assert.eq(col.find({"a": NumberLong("-1")}).count(), 4, "A2");
    assert.eq(col.find({"a": NumberInt("-1")}).count(), 4, "A3");
    assert.eq(col.find({"a": NumberDecimal("-1")}).count(), 4, "A4");

    assert.eq(col.find({"a": NaN}).count(), 3, "B1");
    assert.eq(col.find({"a": NumberDecimal("NaN")}).count(), 3, "B2");
    assert.eq(col.find({"a": Infinity}).count(), 2, "B3");
    assert.eq(col.find({"a": NumberDecimal("Infinity")}).count(), 2, "B4");

    assert.eq(col.find({$and: [{"a": {$gte: 0}}, {"a": {$lte: 2}}]}).count(), 14, "C1");

    // Proper mixed ordering of decimals and doubles
    col.drop();
    assert.writeOK(col.insert([{"a": NumberDecimal("0.3")}, {"a": 0.3}], "2 insertion failed"));

    assert.eq(col.find({"a": {$lt: NumberDecimal("0.3")}}).count(), 1, "D1");
    assert.eq(col.find({"a": {$gt: 0.3}}).count(), 1, "D1");

    // Find with NumberLong, but not Double
    col.drop();
    assert.writeOK(col.insert([{"a": NumberDecimal("36028797018963967")}], "3 insertion failed"));

    assert.eq(col.find({"a": NumberDecimal("36028797018963967")}).count(), 1, "E1");
    // Not representable as double
    assert.eq(col.find({"a": 36028797018963967}).count(), 0, "E2");
    assert.eq(col.find({"a": NumberLong("36028797018963967")}).count(), 1, "E3");

    // Doubles not representable as decimal
    col.drop();
    var exactDouble = 1427247692705959881058285969449495136382746624;
    var exactDoubleString = "1427247692705959881058285969449495136382746624";

    assert.writeOK(col.insert([{"a": exactDouble}  // Exact double (46 digits)
    ],
                              "4 insertion failed"));

    assert.eq(col.find({"a": NumberDecimal(exactDoubleString)}).count(), 0, "F1");
    assert.eq(col.find({"a": {$gt: NumberDecimal(exactDoubleString)}}).count(), 1, "E2");

    var exactDoubleTiny = 1 / 1606938044258990275541962092341162602522202993782792835301376;
    var exactDoubleTinyString =
        "0.00000000000000000000000000000000000000000000000000000000000062230152778611417071440640537801242405902521687211671331011166147896988340353834411839448231257136169569665895551224821247160434722900390625";

    col.drop();
    assert.writeOK(col.insert([{"a": exactDoubleTiny}], "5 insertion failed"));

    assert.eq(col.find({"a": NumberDecimal(exactDoubleTinyString)}).count(), 0, "F1");
    assert.eq(col.find({"a": {$gt: NumberDecimal(exactDoubleTinyString)}}).count(), 1, "F2");
}());
