/**
 * Derived from test cases at https://github.com/mongodb/specifications
 */

let parseErrors = [
    {
        "description": "[basx572] Near-specials " + "(Conversion_syntax)",
        "string": "-9Inf",
    },
    {
        "description": "[basx516] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "-1-",
    },
    {
        "description": "[basx533] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "0000..",
    },
    {
        "description": "[basx534] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": ".0000.",
    },
    {
        "description": "[basx535] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "00..00",
    },
    {
        "description": "[basx569] Near-specials " + "(Conversion_syntax)",
        "string": "0Inf",
    },
    {
        "description": "[basx571] Near-specials " + "(Conversion_syntax)",
        "string": "-0Inf",
    },
    {
        "description": "[basx575] Near-specials " + "(Conversion_syntax)",
        "string": "0sNaN",
    },
    {
        "description": "[basx503] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "++1",
    },
    {
        "description": "[basx504] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "--1",
    },
    {
        "description": "[basx505] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "-+1",
    },
    {
        "description": "[basx506] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "+-1",
    },
    {
        "description": "[basx510] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": " +1",
    },
    {
        "description": "[basx513] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": " + 1",
    },
    {
        "description": "[basx514] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": " - 1",
    },
    {
        "description": "[basx501] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": ".",
    },
    {
        "description": "[basx502] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "..",
    },
    {
        "description": "[basx519] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "",
    },
    {
        "description": "[basx525] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "e100",
    },
    {
        "description": "[basx549] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "e+1",
    },
    {
        "description": "[basx577] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": ".e+1",
    },
    {
        "description": "[basx578] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": "+.e+1",
    },
    {
        "description": "[basx581] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": "E+1",
    },
    {
        "description": "[basx582] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": ".E+1",
    },
    {
        "description": "[basx583] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": "+.E+1",
    },
    {
        "description": "[basx579] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": "-.e+",
    },
    {
        "description": "[basx580] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": "-.e",
    },
    {
        "description": "[basx584] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": "-.E+",
    },
    {
        "description": "[basx585] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": "-.E",
    },
    {
        "description": "[basx589] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": "+.Inf",
    },
    {
        "description": "[basx586] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": ".NaN",
    },
    {
        "description": "[basx587] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": "-.NaN",
    },
    {
        "description": "[basx545] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "ONE",
    },
    {
        "description": "[basx561] Near-specials " + "(Conversion_syntax)",
        "string": "qNaN",
    },
    {
        "description": "[basx573] Near-specials " + "(Conversion_syntax)",
        "string": "-sNa",
    },
    {
        "description": "[basx588] some baddies with dots and Es and dots and specials " + "(Conversion_syntax)",
        "string": "+.sNaN",
    },
    {
        "description": "[basx544] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "ten",
    },
    {
        "description": "[basx527] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "u0b65",
    },
    {
        "description": "[basx526] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "u0e5a",
    },
    {
        "description": "[basx515] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "x",
    },
    {
        "description": "[basx574] Near-specials " + "(Conversion_syntax)",
        "string": "xNaN",
    },
    {
        "description": "[basx530] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": ".123.5",
    },
    {
        "description": "[basx500] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1..2",
    },
    {
        "description": "[basx542] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1e1.0",
    },
    {
        "description": "[basx553] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1E+1.2.3",
    },
    {
        "description": "[basx543] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1e123e",
    },
    {
        "description": "[basx552] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1E+1.2",
    },
    {
        "description": "[basx546] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1e.1",
    },
    {
        "description": "[basx547] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1e1.",
    },
    {
        "description": "[basx554] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1E++1",
    },
    {
        "description": "[basx555] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1E--1",
    },
    {
        "description": "[basx556] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1E+-1",
    },
    {
        "description": "[basx557] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1E-+1",
    },
    {
        "description": "[basx558] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1E'1",
    },
    {
        "description": "[basx559] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": '1E"1',
    },
    {
        "description": "[basx520] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1e-",
    },
    {
        "description": "[basx560] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1E",
    },
    {
        "description": "[basx548] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1ee",
    },
    {
        "description": "[basx551] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1.2.1",
    },
    {
        "description": "[basx550] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1.23.4",
    },
    {
        "description": "[basx529] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "1.34.5",
    },
    {
        "description": "[basx531] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "01.35.",
    },
    {
        "description": "[basx532] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "01.35-",
    },
    {
        "description": "[basx518] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "3+",
    },
    {
        "description": "[basx521] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "7e99999a",
    },
    {
        "description": "[basx570] Near-specials " + "(Conversion_syntax)",
        "string": "9Inf",
    },
    {
        "description": "[basx512] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "12 ",
    },
    {
        "description": "[basx517] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "12-",
    },
    {
        "description": "[basx507] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "12e",
    },
    {
        "description": "[basx508] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "12e++",
    },
    {
        "description": "[basx509] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "12f4",
    },
    {
        "description": "[basx536] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "111e*123",
    },
    {
        "description": "[basx537] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "111e123-",
    },
    {
        "description": "[basx540] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "111e1*23",
    },
    {
        "description": "[basx538] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "111e+12+",
    },
    {
        "description": "[basx539] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "111e1-3-",
    },
    {
        "description": "[basx541] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "111E1e+3",
    },
    {
        "description": "[basx528] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "123,65",
    },
    {
        "description": "[basx523] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "7e12356789012x",
    },
    {
        "description": "[basx522] The 'baddies' tests from DiagBigDecimal, plus some new ones " + "(Conversion_syntax)",
        "string": "7e123567890x",
    },
];

parseErrors.forEach(function (testCase) {
    print(`Test - ${testCase.description}`);
    function test() {
        NumberDecimal(testCase.string);
    }
    assert.throws(test, [], `[Test - ${testCase.description}] should have failed with error.`);
});
