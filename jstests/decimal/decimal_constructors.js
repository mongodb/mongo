// Tests constructing NumberDecimal with various types

(function() {
    "use strict";
    var col = db.decimal_constructors;
    col.drop();

    // Insert some sample data.

    assert.writeOK(col.insert([
        {"decimal": NumberDecimal("1")},
        {"decimal": NumberDecimal(1)},
        {"decimal": NumberDecimal(NumberLong("1"))},
        {"decimal": NumberDecimal(NumberInt("1"))},
    ]),
                   "Initial insertion of decimals failed");

    // Find values with various types and NumberDecimal constructed types
    assert.eq(col.find({"decimal": NumberDecimal("1")}).count(), "4");
    assert.eq(col.find({"decimal": NumberDecimal(1)}).count(), "4");
    assert.eq(col.find({"decimal": NumberDecimal(NumberLong(1))}).count(), "4");
    assert.eq(col.find({"decimal": NumberDecimal(NumberInt(1))}).count(), "4");
    assert.eq(col.find({"decimal": 1}).count(), "4");
    assert.eq(col.find({"decimal": NumberLong(1)}).count(), "4");
    assert.eq(col.find({"decimal": NumberInt(1)}).count(), "4");
}());
