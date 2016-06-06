/**
 * Derived from test cases at https://github.com/mongodb/specifications
 */

(function() {
    "use strict";

    var parseErrors = [
        {"description": "Incomplete Exponent", "string": "1e"},
        {"description": "Exponent at the beginning", "string": "E01"},
        {"description": "Just a decimal place", "string": "."},
        {"description": "2 decimal places", "string": "..3"},
        {"description": "2 decimal places", "string": ".13.3"},
        {"description": "2 decimal places", "string": "1..3"},
        {"description": "2 decimal places", "string": "1.3.4"},
        {"description": "2 decimal places", "string": "1.34."},
        {"description": "Decimal with no digits", "string": ".e"},
        {"description": "2 signs", "string": "+-32.4"},
        {"description": "2 signs", "string": "-+32.4"},
        {"description": "2 negative signs", "string": "--32.4"},
        {"description": "2 negative signs", "string": "-32.-4"},
        {"description": "End in negative sign", "string": "32.0-"},
        {"description": "2 negative signs", "string": "32.4E--21"},
        {"description": "2 negative signs", "string": "32.4E-2-1"},
        {"description": "2 signs", "string": "32.4E+-21"},
        {"description": "Empty string", "string": ""},
        {"description": "leading white space positive number", "string": " 1"},
        {"description": "leading white space negative number", "string": " -1"},
        {"description": "trailing white space", "string": "1 "},
        {"description": "Invalid", "string": "E"},
        {"description": "Invalid", "string": "invalid"},
        {"description": "Invalid", "string": "i"},
        {"description": "Invalid", "string": "in"},
        {"description": "Invalid", "string": "-in"},
        {"description": "Invalid", "string": "Na"},
        {"description": "Invalid", "string": "-Na"},
        {"description": "Invalid", "string": "1.23abc"},
        {"description": "Invalid", "string": "1.23abcE+02"},
        {"description": "Invalid", "string": "1.23E+0aabs2"}
    ];

    parseErrors.forEach(function(testCase) {
        print(`Test - ${testCase.description}`);
        function test() {
            NumberDecimal(testCase.string);
        }
        assert.throws(test, [], `[Test - ${testCase.description}] should have failed with error.`);
    });
}());