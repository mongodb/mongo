/**
 * Derived from test cases at https://github.com/mongodb/specifications
 */

(function() {
    "use strict";

    var testData = [
        {"description": "Special - Canonical NaN", "input": "NaN"},
        {"description": "Special - Negative NaN", "input": "NaN", "lossy": true},
        {
          "description": "Special - Negative NaN",
          "expected": "NaN",
          "input": "-NaN",
          "lossy": true
        },
        {"description": "Special - Canonical SNaN", "input": "NaN", "lossy": true},
        {"description": "Special - Negative SNaN", "input": "NaN", "lossy": true},
        {"description": "Special - NaN with a payload", "input": "NaN", "lossy": true},
        {"description": "Special - Canonical Positive Infinity", "input": "Infinity"},
        {"description": "Special - Canonical Negative Infinity", "input": "-Infinity"},
        {
          "description": "Special - Invalid representation treated as 0",
          "input": "0",
          "lossy": true
        },
        {
          "description": "Special - Invalid representation treated as -0",
          "input": "-0",
          "lossy": true
        },
        {
          "description": "Special - Invalid representation treated as 0E3",
          "input": "0E+3",
          "lossy": true
        },
        {
          "description": "Regular - Adjusted Exponent Limit",
          "input": "0.000001234567890123456789012345678901234"
        },
        {"description": "Regular - Smallest", "input": "0.001234"},
        {"description": "Regular - Smallest with Trailing Zeros", "input": "0.00123400000"},
        {"description": "Regular - 0.1", "input": "0.1"},
        {
          "description": "Regular - 0.1234567890123456789012345678901234",
          "input": "0.1234567890123456789012345678901234"
        },
        {"description": "Regular - 0", "input": "0"},
        {"description": "Regular - -0", "input": "-0"},
        {"description": "Regular - -0.0", "input": "-0.0"},
        {"description": "Regular - 2", "input": "2"},
        {"description": "Regular - 2.000", "input": "2.000"},
        {"description": "Regular - Largest", "input": "1234567890123456789012345678901234"},
        {
          "description": "Scientific - Tiniest",
          "input": "9.999999999999999999999999999999999E-6143"
        },
        {"description": "Scientific - Tiny", "input": "1E-6176"},
        {"description": "Scientific - Negative Tiny", "input": "-1E-6176"},
        {
          "description": "Scientific - Adjusted Exponent Limit",
          "input": "1.234567890123456789012345678901234E-7"
        },
        {"description": "Scientific - Fractional", "input": "-1.00E-8"},
        {"description": "Scientific - 0 with Exponent", "input": "0E+6000"},
        {"description": "Scientific - 0 with Negative Exponent", "input": "0E-611"},
        {"description": "Scientific - No Decimal with Signed Exponent", "input": "1E+3"},
        {"description": "Scientific - Trailing Zero", "input": "1.050E+4"},
        {"description": "Scientific - With Decimal", "input": "1.05E+3"},
        {"description": "Scientific - Full", "input": "5192296858534827628530496329220095"},
        {"description": "Scientific - Large", "input": "1.000000000000000000000000000000000E+6144"},
        {
          "description": "Scientific - Largest",
          "input": "9.999999999999999999999999999999999E+6144"
        },
        {
          "description": "Non-Canonical Parsing - Exponent Normalization",
          "input": "-100E-10",
          "expected": "-1.00E-8"
        },
        {
          "description": "Non-Canonical Parsing - Unsigned Positive Exponent",
          "input": "1E3",
          "expected": "1E+3"
        },
        {
          "description": "Non-Canonical Parsing - Lowercase Exponent Identifier",
          "input": "1e+3",
          "expected": "1E+3"
        },
        {
          "description": "Non-Canonical Parsing - Long Significand with Exponent",
          "input": "12345689012345789012345E+12",
          "expected": "1.2345689012345789012345E+34"
        },
        {
          "description": "Non-Canonical Parsing - Positive Sign",
          "input": "+1234567890123456789012345678901234",
          "expected": "1234567890123456789012345678901234"
        },
        {
          "description": "Non-Canonical Parsing - Long Decimal String",
          "input":
              ".0000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "00000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "00000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "00000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "00000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "00000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "00000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "00000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "00000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "00000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "00000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "00000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "0000000000000000000000000000000000000001",
          "expected": "1E-999"
        },
        {"description": "Non-Canonical Parsing - nan", "input": "nan", "expected": "NaN"},
        {"description": "Non-Canonical Parsing - nAn", "input": "nAn", "expected": "NaN"},
        {
          "description": "Non-Canonical Parsing - +infinity",
          "input": "+infinity",
          "expected": "Infinity"
        },
        {
          "description": "Non-Canonical Parsing - infinity",
          "input": "infinity",
          "expected": "Infinity"
        },
        {
          "description": "Non-Canonical Parsing - infiniTY",
          "input": "infiniTY",
          "expected": "Infinity"
        },
        {"description": "Non-Canonical Parsing - inf", "input": "inf", "expected": "Infinity"},
        {"description": "Non-Canonical Parsing - inF", "input": "inF", "expected": "Infinity"},
        {
          "description": "Non-Canonical Parsing - -infinity",
          "input": "-infinity",
          "expected": "-Infinity"
        },
        {
          "description": "Non-Canonical Parsing - -infiniTy",
          "input": "-infiniTy",
          "expected": "-Infinity"
        },
        {
          "description": "Non-Canonical Parsing - -Inf",
          "input": "-Infinity",
          "expected": "-Infinity"
        },
        {"description": "Non-Canonical Parsing - -inf", "input": "-inf", "expected": "-Infinity"},
        {"description": "Non-Canonical Parsing - -inF", "input": "-inF", "expected": "-Infinity"},
        {"description": "Rounded Subnormal number", "input": "10E-6177", "expected": "1E-6176"},
        {"description": "Clamped", "input": "1E6112", "expected": "1.0E+6112"},
        {
          "description": "Exact rounding",
          "input":
              "100000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "000000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "000000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "000000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "000000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "000000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "000000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "000000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "000000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "000000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "000000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "000000000000000000000000000000000000000000000000000000000000000000000000000000000" +
              "0000000000000000000000000000",
          "expected": "1.000000000000000000000000000000000E+999"
        }
    ];

    testData.forEach(function(testCase) {
        print(`Test - ${testCase.description}`);
        var output = NumberDecimal(testCase.input).toString();
        if (testCase.expected) {
            assert.eq(output, `NumberDecimal("${testCase.expected}")`);
        } else {
            assert.eq(output, `NumberDecimal("${testCase.input}")`);
        }
    });
}());