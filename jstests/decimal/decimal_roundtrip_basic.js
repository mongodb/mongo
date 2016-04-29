// Tests doing simple round-trip operations from the shell.

(function() {
    "use strict";
    var col = db.roundtrip_basic;
    col.drop();

    // Insert some sample data.

    assert.writeOK(col.insert([
        {"decimal": NumberDecimal("0")},
        {"decimal": NumberDecimal("0.00")},
        {"decimal": NumberDecimal("-0")},
        {"decimal": NumberDecimal("1.0")},
        {"decimal": NumberDecimal("0.10")},
        {"decimal": NumberDecimal("2.00")},
        {"decimal": NumberDecimal("12345678901234567890.12345678901234")},
        {"decimal": NumberDecimal("NaN")},
        {"decimal": NumberDecimal("-NaN")},
        {"decimal": NumberDecimal("Infinity")},
        {"decimal": NumberDecimal("-Infinity")},
        {"decimal": NumberDecimal("9999999999999999999999999999999999E6111")},
        {"decimal": NumberDecimal("1E-6176")},
    ]),
                   "Initial insertion of decimals failed");

    // Check that the searching for queryValue results in finding expectedValues.
    // All arguments are string representations of NumberDecimal values.
    function checkDecimals(queryValue, expectedValues) {
        queryValue = NumberDecimal(queryValue);
        expectedValues = expectedValues.map((function(string) {
            return NumberDecimal(string);
        }));
        var docs = col.find({decimal: queryValue}, {_id: 0}).sort({decimal: 1, _id: 1}).toArray();
        var actualValues = docs.map((function(item) {
            return item.decimal;
        }));
        assert.eq(actualValues, expectedValues, "problem retrieving " + queryValue.toString());
    }

    checkDecimals("0", ["0", "0.00", "-0"]);
    checkDecimals("1.0", ["1.0"]);
    checkDecimals("0.1", ["0.10"]);
    checkDecimals("2", ["2.00"]);
    checkDecimals("12345678901234567890.12345678901234", ["12345678901234567890.12345678901234"]);
    checkDecimals("NaN", ["NaN", "-NaN"]);
    checkDecimals("Infinity", ["Infinity"]);
    checkDecimals("-Infinity", ["-Infinity"]);
    checkDecimals("9999999999999999999999999999999999E6111",
                  ["9999999999999999999999999999999999E6111"]);
    checkDecimals("1E-6176", ["1E-6176"]);
}());
