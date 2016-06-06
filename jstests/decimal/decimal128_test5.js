/**
 * Derived from test cases at https://github.com/mongodb/specifications
 */

(function() {
    "use strict";

    var testData = [
        {
          "description": "[decq035] fold-downs (more below) (Clamped)",
          "input": "1.23E+6144",
          "expected": "1.230000000000000000000000000000000E+6144"
        },
        {
          "description": "[decq037] fold-downs (more below) (Clamped)",
          "input": "1E+6144",
          "expected": "1.000000000000000000000000000000000E+6144"
        },
        {
          "description": "[decq077] Nmin and below (Subnormal)",
          "input": "0.100000000000000000000000000000000E-6143",
          "expected": "1.00000000000000000000000000000000E-6144"
        },
        {
          "description": "[decq078] Nmin and below (Subnormal)",
          "input": "1.00000000000000000000000000000000E-6144"
        },
        {
          "description": "[decq079] Nmin and below (Subnormal)",
          "input": "0.000000000000000000000000000000010E-6143",
          "expected": "1.0E-6175"
        },
        {"description": "[decq080] Nmin and below (Subnormal)", "input": "1.0E-6175"},
        {
          "description": "[decq081] Nmin and below (Subnormal)",
          "input": "0.00000000000000000000000000000001E-6143",
          "expected": "1E-6175"
        },
        {"description": "[decq082] Nmin and below (Subnormal)", "input": "1E-6175"},
        {
          "description": "[decq083] Nmin and below (Subnormal)",
          "input": "0.000000000000000000000000000000001E-6143",
          "expected": "1E-6176"
        },
        {"description": "[decq084] Nmin and below (Subnormal)", "input": "1E-6176"},
        {
          "description": "[decq090] underflows cannot be tested for simple copies, " +
              "check edge cases (Subnormal)",
          "input": "1e-6176",
          "expected": "1E-6176"
        },
        {
          "description": "[decq100] underflows cannot be tested for simple copies, " +
              "check edge cases (Subnormal)",
          "input": "999999999999999999999999999999999e-6176",
          "expected": "9.99999999999999999999999999999999E-6144"
        },
        {
          "description": "[decq130] fold-downs (more below) (Clamped)",
          "input": "-1.23E+6144",
          "expected": "-1.230000000000000000000000000000000E+6144"
        },
        {
          "description": "[decq132] fold-downs (more below) (Clamped)",
          "input": "-1E+6144",
          "expected": "-1.000000000000000000000000000000000E+6144"
        },
        {
          "description": "[decq177] Nmin and below (Subnormal)",
          "input": "-0.100000000000000000000000000000000E-6143",
          "expected": "-1.00000000000000000000000000000000E-6144"
        },
        {
          "description": "[decq178] Nmin and below (Subnormal)",
          "input": "-1.00000000000000000000000000000000E-6144"
        },
        {
          "description": "[decq179] Nmin and below (Subnormal)",
          "input": "-0.000000000000000000000000000000010E-6143",
          "expected": "-1.0E-6175"
        },
        {"description": "[decq180] Nmin and below (Subnormal)", "input": "-1.0E-6175"},
        {
          "description": "[decq181] Nmin and below (Subnormal)",
          "input": "-0.00000000000000000000000000000001E-6143",
          "expected": "-1E-6175"
        },
        {"description": "[decq182] Nmin and below (Subnormal)", "input": "-1E-6175"},
        {
          "description": "[decq183] Nmin and below (Subnormal)",
          "input": "-0.000000000000000000000000000000001E-6143",
          "expected": "-1E-6176"
        },
        {"description": "[decq184] Nmin and below (Subnormal)", "input": "-1E-6176"},
        {
          "description": "[decq190] underflow edge cases (Subnormal)",
          "input": "-1e-6176",
          "expected": "-1E-6176"
        },
        {
          "description": "[decq200] underflow edge cases (Subnormal)",
          "input": "-999999999999999999999999999999999e-6176",
          "expected": "-9.99999999999999999999999999999999E-6144"
        },
        {"description": "[decq400] zeros (Clamped)", "input": "0E-8000", "expected": "0E-6176"},
        {"description": "[decq401] zeros (Clamped)", "input": "0E-6177", "expected": "0E-6176"},
        {
          "description": "[decq414] clamped zeros... (Clamped)",
          "input": "0E+6112",
          "expected": "0E+6111"
        },
        {
          "description": "[decq416] clamped zeros... (Clamped)",
          "input": "0E+6144",
          "expected": "0E+6111"
        },
        {
          "description": "[decq418] clamped zeros... (Clamped)",
          "input": "0E+8000",
          "expected": "0E+6111"
        },
        {
          "description": "[decq420] negative zeros (Clamped)",
          "input": "-0E-8000",
          "expected": "-0E-6176"
        },
        {
          "description": "[decq421] negative zeros (Clamped)",
          "input": "-0E-6177",
          "expected": "-0E-6176"
        },
        {
          "description": "[decq434] clamped zeros... (Clamped)",
          "input": "-0E+6112",
          "expected": "-0E+6111"
        },
        {
          "description": "[decq436] clamped zeros... (Clamped)",
          "input": "-0E+6144",
          "expected": "-0E+6111"
        },
        {
          "description": "[decq438] clamped zeros... (Clamped)",
          "input": "-0E+8000",
          "expected": "-0E+6111"
        },
        {
          "description": "[decq601] fold-down full sequence (Clamped)",
          "input": "1E+6144",
          "expected": "1.000000000000000000000000000000000E+6144"
        },
        {
          "description": "[decq603] fold-down full sequence (Clamped)",
          "input": "1E+6143",
          "expected": "1.00000000000000000000000000000000E+6143"
        },
        {
          "description": "[decq605] fold-down full sequence (Clamped)",
          "input": "1E+6142",
          "expected": "1.0000000000000000000000000000000E+6142"
        },
        {
          "description": "[decq607] fold-down full sequence (Clamped)",
          "input": "1E+6141",
          "expected": "1.000000000000000000000000000000E+6141"
        },
        {
          "description": "[decq609] fold-down full sequence (Clamped)",
          "input": "1E+6140",
          "expected": "1.00000000000000000000000000000E+6140"
        },
        {
          "description": "[decq611] fold-down full sequence (Clamped)",
          "input": "1E+6139",
          "expected": "1.0000000000000000000000000000E+6139"
        },
        {
          "description": "[decq613] fold-down full sequence (Clamped)",
          "input": "1E+6138",
          "expected": "1.000000000000000000000000000E+6138"
        },
        {
          "description": "[decq615] fold-down full sequence (Clamped)",
          "input": "1E+6137",
          "expected": "1.00000000000000000000000000E+6137"
        },
        {
          "description": "[decq617] fold-down full sequence (Clamped)",
          "input": "1E+6136",
          "expected": "1.0000000000000000000000000E+6136"
        },
        {
          "description": "[decq619] fold-down full sequence (Clamped)",
          "input": "1E+6135",
          "expected": "1.000000000000000000000000E+6135"
        },
        {
          "description": "[decq621] fold-down full sequence (Clamped)",
          "input": "1E+6134",
          "expected": "1.00000000000000000000000E+6134"
        },
        {
          "description": "[decq623] fold-down full sequence (Clamped)",
          "input": "1E+6133",
          "expected": "1.0000000000000000000000E+6133"
        },
        {
          "description": "[decq625] fold-down full sequence (Clamped)",
          "input": "1E+6132",
          "expected": "1.000000000000000000000E+6132"
        },
        {
          "description": "[decq627] fold-down full sequence (Clamped)",
          "input": "1E+6131",
          "expected": "1.00000000000000000000E+6131"
        },
        {
          "description": "[decq629] fold-down full sequence (Clamped)",
          "input": "1E+6130",
          "expected": "1.0000000000000000000E+6130"
        },
        {
          "description": "[decq631] fold-down full sequence (Clamped)",
          "input": "1E+6129",
          "expected": "1.000000000000000000E+6129"
        },
        {
          "description": "[decq633] fold-down full sequence (Clamped)",
          "input": "1E+6128",
          "expected": "1.00000000000000000E+6128"
        },
        {
          "description": "[decq635] fold-down full sequence (Clamped)",
          "input": "1E+6127",
          "expected": "1.0000000000000000E+6127"
        },
        {
          "description": "[decq637] fold-down full sequence (Clamped)",
          "input": "1E+6126",
          "expected": "1.000000000000000E+6126"
        },
        {
          "description": "[decq639] fold-down full sequence (Clamped)",
          "input": "1E+6125",
          "expected": "1.00000000000000E+6125"
        },
        {
          "description": "[decq641] fold-down full sequence (Clamped)",
          "input": "1E+6124",
          "expected": "1.0000000000000E+6124"
        },
        {
          "description": "[decq643] fold-down full sequence (Clamped)",
          "input": "1E+6123",
          "expected": "1.000000000000E+6123"
        },
        {
          "description": "[decq645] fold-down full sequence (Clamped)",
          "input": "1E+6122",
          "expected": "1.00000000000E+6122"
        },
        {
          "description": "[decq647] fold-down full sequence (Clamped)",
          "input": "1E+6121",
          "expected": "1.0000000000E+6121"
        },
        {
          "description": "[decq649] fold-down full sequence (Clamped)",
          "input": "1E+6120",
          "expected": "1.000000000E+6120"
        },
        {
          "description": "[decq651] fold-down full sequence (Clamped)",
          "input": "1E+6119",
          "expected": "1.00000000E+6119"
        },
        {
          "description": "[decq653] fold-down full sequence (Clamped)",
          "input": "1E+6118",
          "expected": "1.0000000E+6118"
        },
        {
          "description": "[decq655] fold-down full sequence (Clamped)",
          "input": "1E+6117",
          "expected": "1.000000E+6117"
        },
        {
          "description": "[decq657] fold-down full sequence (Clamped)",
          "input": "1E+6116",
          "expected": "1.00000E+6116"
        },
        {
          "description": "[decq659] fold-down full sequence (Clamped)",
          "input": "1E+6115",
          "expected": "1.0000E+6115"
        },
        {
          "description": "[decq661] fold-down full sequence (Clamped)",
          "input": "1E+6114",
          "expected": "1.000E+6114"
        },
        {
          "description": "[decq663] fold-down full sequence (Clamped)",
          "input": "1E+6113",
          "expected": "1.00E+6113"
        },
        {
          "description": "[decq665] fold-down full sequence (Clamped)",
          "input": "1E+6112",
          "expected": "1.0E+6112"
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
