/**
 * Derived from test cases at https://github.com/mongodb/specifications
 */

(function() {
    "use strict";

    var testData = [
        {
          "description": "[basx023] conform to rules and exponent will be in permitted range).",
          "input": "-0.1"
        },

        {
          "description": "[basx045] strings without E cannot generate E in result",
          "input": "+0.003",
          "expected": "0.003"
        },
        {"description": "[basx610] Zeros", "input": ".0", "expected": "0.0"},
        {"description": "[basx612] Zeros", "input": "-.0", "expected": "-0.0"},
        {
          "description": "[basx043] strings without E cannot generate E in result",
          "input": "+12.76",
          "expected": "12.76"
        },
        {
          "description": "[basx055] strings without E cannot generate E in result",
          "input": "0.00000005",
          "expected": "5E-8"
        },
        {
          "description": "[basx054] strings without E cannot generate E in result",
          "input": "0.0000005",
          "expected": "5E-7"
        },
        {
          "description": "[basx052] strings without E cannot generate E in result",
          "input": "0.000005"
        },
        {
          "description": "[basx051] strings without E cannot generate E in result",
          "input": "00.00005",
          "expected": "0.00005"
        },
        {
          "description": "[basx050] strings without E cannot generate E in result",
          "input": "0.0005"
        },
        {
          "description": "[basx047] strings without E cannot generate E in result",
          "input": ".5",
          "expected": "0.5"
        },
        {
          "description": "[dqbsr431] check rounding modes heeded (Rounded)",
          "input": "1.1111111111111111111111111111123450",
          "expected": "1.111111111111111111111111111112345"
        },
        {
          "description": "OK2",
          "input": ".100000000000000000000000000000000000000000000000000000000000",
          "expected": "0.1000000000000000000000000000000000"
        }
    ];

    var parseErrors = [
        {"description": "[basx564] Near-specials (Conversion_syntax)", "string": "Infi"},
        {"description": "[basx565] Near-specials (Conversion_syntax)", "string": "Infin"},
        {"description": "[basx566] Near-specials (Conversion_syntax)", "string": "Infini"},
        {"description": "[basx567] Near-specials (Conversion_syntax)", "string": "Infinit"},
        {"description": "[basx568] Near-specials (Conversion_syntax)", "string": "-Infinit"},
        {
          "description":
              "[basx590] some baddies with dots and Es and dots and specials (Conversion_syntax)",
          "string": ".Infinity"
        },
        {"description": "[basx562] Near-specials (Conversion_syntax)", "string": "NaNq"},
        {"description": "[basx563] Near-specials (Conversion_syntax)", "string": "NaNs"},
        {
          "description": "[dqbas939] overflow results at different rounding modes " +
              "(Overflow & Inexact & Rounded)",
          "string": "-7e10000"
        },
        {
          "description": "[dqbsr534] negatives (Rounded & Inexact)",
          "string": "-1.11111111111111111111111111111234650"
        },
        {
          "description": "[dqbsr535] negatives (Rounded & Inexact)",
          "string": "-1.11111111111111111111111111111234551"
        },
        {
          "description": "[dqbsr533] negatives (Rounded & Inexact)",
          "string": "-1.11111111111111111111111111111234550"
        },
        {
          "description": "[dqbsr532] negatives (Rounded & Inexact)",
          "string": "-1.11111111111111111111111111111234549"
        },
        {
          "description": "[dqbsr432] check rounding modes heeded (Rounded & Inexact)",
          "string": "1.11111111111111111111111111111234549"
        },
        {
          "description": "[dqbsr433] check rounding modes heeded (Rounded & Inexact)",
          "string": "1.11111111111111111111111111111234550"
        },
        {
          "description": "[dqbsr435] check rounding modes heeded (Rounded & Inexact)",
          "string": "1.11111111111111111111111111111234551"
        },
        {
          "description": "[dqbsr434] check rounding modes heeded (Rounded & Inexact)",
          "string": "1.11111111111111111111111111111234650"
        },
        {
          "description": "[dqbas938] overflow results at different rounding modes " +
              "(Overflow & Inexact & Rounded)",
          "string": "7e10000"
        },
        {
          "description": "Inexact rounding#1",
          "string": "100000000000000000000000000000000000000000000000000000000001"
        },
        {"description": "Inexact rounding#2", "string": "1E-6177"}
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

    parseErrors.forEach(function(testCase) {
        print(`Test - ${testCase.description}`);
        function test() {
            NumberDecimal(testCase.string);
        }
        assert.throws(test, [], `[Test - ${testCase.description}] should have failed with error.`);
    });
}());