/**
 * Derived from test cases at https://github.com/mongodb/specifications
 */

(function() {
    var data = [
        {"description": "[decq021] Normality", "input": "-1234567890123456789012345678901234"},
        {
          "description": "[decq823] values around [u]int32 edges (zeros done earlier)",
          "input": "-2147483649"
        },
        {
          "description": "[decq822] values around [u]int32 edges (zeros done earlier)",
          "input": "-2147483648"
        },
        {
          "description": "[decq821] values around [u]int32 edges (zeros done earlier)",
          "input": "-2147483647"
        },
        {
          "description": "[decq820] values around [u]int32 edges (zeros done earlier)",
          "input": "-2147483646"
        },
        {"description": "[decq152] fold-downs (more below)", "input": "-12345"},
        {"description": "[decq154] fold-downs (more below)", "input": "-1234"},
        {"description": "[decq006] derivative canonical plain strings", "input": "-750"},
        {"description": "[decq164] fold-downs (more below)", "input": "-123.45"},
        {"description": "[decq156] fold-downs (more below)", "input": "-123"},
        {"description": "[decq008] derivative canonical plain strings", "input": "-75.0"},
        {"description": "[decq158] fold-downs (more below)", "input": "-12"},
        {
          "description": "[decq122] Nmax and similar",
          "input": "-9.999999999999999999999999999999999E+6144"
        },
        {
          "description": "[decq002] (mostly derived from the Strawman 4 document and examples)",
          "input": "-7.50"
        },
        {"description": "[decq004] derivative canonical plain strings", "input": "-7.50E+3"},
        {"description": "[decq018] derivative canonical plain strings", "input": "-7.50E-7"},
        {
          "description": "[decq125] Nmax and similar",
          "input": "-1.234567890123456789012345678901234E+6144"
        },
        {
          "description": "[decq131] fold-downs (more below)",
          "input": "-1.230000000000000000000000000000000E+6144"
        },
        {"description": "[decq162] fold-downs (more below)", "input": "-1.23"},
        {
          "description": "[decq176] Nmin and below",
          "input": "-1.000000000000000000000000000000001E-6143"
        },
        {
          "description": "[decq174] Nmin and below",
          "input": "-1.000000000000000000000000000000000E-6143"
        },
        {
          "description": "[decq133] fold-downs (more below)",
          "input": "-1.000000000000000000000000000000000E+6144"
        },
        {"description": "[decq160] fold-downs (more below)", "input": "-1"},
        {"description": "[decq172] Nmin and below", "input": "-1E-6143"},
        {"description": "[decq010] derivative canonical plain strings", "input": "-0.750"},
        {"description": "[decq012] derivative canonical plain strings", "input": "-0.0750"},
        {"description": "[decq014] derivative canonical plain strings", "input": "-0.000750"},
        {"description": "[decq016] derivative canonical plain strings", "input": "-0.00000750"},
        {"description": "[decq404] zeros", "input": "0E-6176"},
        {"description": "[decq424] negative zeros", "input": "-0E-6176"},
        {"description": "[decq407] zeros", "input": "0.00"},
        {"description": "[decq427] negative zeros", "input": "-0.00"},
        {"description": "[decq409] zeros", "input": "0"},
        {"description": "[decq428] negative zeros", "input": "-0"},
        {"description": "[decq700] Selected DPD codes", "input": "0"},
        {"description": "[decq406] zeros", "input": "0.00"},
        {"description": "[decq426] negative zeros", "input": "-0.00"},
        {"description": "[decq410] zeros", "input": "0E+3"},
        {"description": "[decq431] negative zeros", "input": "-0E+3"},
        {"description": "[decq419] clamped zeros...", "input": "0E+6111"},
        {"description": "[decq432] negative zeros", "input": "-0E+6111"},
        {"description": "[decq405] zeros", "input": "0E-6176"},
        {"description": "[decq425] negative zeros", "input": "-0E-6176"},
        {"description": "[decq508] Specials", "input": "Infinity"},
        {"description": "[decq528] Specials", "input": "-Infinity"},
        {"description": "[decq541] Specials", "input": "NaN"},
        {
          "description": "[decq074] Nmin and below",
          "input": "1.000000000000000000000000000000000E-6143"
        },
        {
          "description": "[decq602] fold-down full sequence",
          "input": "1.000000000000000000000000000000000E+6144"
        },
        {
          "description": "[decq604] fold-down full sequence",
          "input": "1.00000000000000000000000000000000E+6143"
        },
        {
          "description": "[decq606] fold-down full sequence",
          "input": "1.0000000000000000000000000000000E+6142"
        },
        {
          "description": "[decq608] fold-down full sequence",
          "input": "1.000000000000000000000000000000E+6141"
        },
        {
          "description": "[decq610] fold-down full sequence",
          "input": "1.00000000000000000000000000000E+6140"
        },
        {
          "description": "[decq612] fold-down full sequence",
          "input": "1.0000000000000000000000000000E+6139"
        },
        {
          "description": "[decq614] fold-down full sequence",
          "input": "1.000000000000000000000000000E+6138"
        },
        {
          "description": "[decq616] fold-down full sequence",
          "input": "1.00000000000000000000000000E+6137"
        },
        {
          "description": "[decq618] fold-down full sequence",
          "input": "1.0000000000000000000000000E+6136"
        },
        {
          "description": "[decq620] fold-down full sequence",
          "input": "1.000000000000000000000000E+6135"
        },
        {
          "description": "[decq622] fold-down full sequence",
          "input": "1.00000000000000000000000E+6134"
        },
        {
          "description": "[decq624] fold-down full sequence",
          "input": "1.0000000000000000000000E+6133"
        },
        {
          "description": "[decq626] fold-down full sequence",
          "input": "1.000000000000000000000E+6132"
        },
        {
          "description": "[decq628] fold-down full sequence",
          "input": "1.00000000000000000000E+6131"
        },
        {
          "description": "[decq630] fold-down full sequence",
          "input": "1.0000000000000000000E+6130"
        },
        {"description": "[decq632] fold-down full sequence", "input": "1.000000000000000000E+6129"},
        {"description": "[decq634] fold-down full sequence", "input": "1.00000000000000000E+6128"},
        {"description": "[decq636] fold-down full sequence", "input": "1.0000000000000000E+6127"},
        {"description": "[decq638] fold-down full sequence", "input": "1.000000000000000E+6126"},
        {"description": "[decq640] fold-down full sequence", "input": "1.00000000000000E+6125"},
        {"description": "[decq642] fold-down full sequence", "input": "1.0000000000000E+6124"},
        {"description": "[decq644] fold-down full sequence", "input": "1.000000000000E+6123"},
        {"description": "[decq646] fold-down full sequence", "input": "1.00000000000E+6122"},
        {"description": "[decq648] fold-down full sequence", "input": "1.0000000000E+6121"},
        {"description": "[decq650] fold-down full sequence", "input": "1.000000000E+6120"},
        {"description": "[decq652] fold-down full sequence", "input": "1.00000000E+6119"},
        {"description": "[decq654] fold-down full sequence", "input": "1.0000000E+6118"},
        {"description": "[decq656] fold-down full sequence", "input": "1.000000E+6117"},
        {"description": "[decq658] fold-down full sequence", "input": "1.00000E+6116"},
        {"description": "[decq660] fold-down full sequence", "input": "1.0000E+6115"},
        {"description": "[decq662] fold-down full sequence", "input": "1.000E+6114"},
        {"description": "[decq664] fold-down full sequence", "input": "1.00E+6113"},
        {"description": "[decq666] fold-down full sequence", "input": "1.0E+6112"},
        {"description": "[decq060] fold-downs (more below)", "input": "1"},
        {"description": "[decq670] fold-down full sequence", "input": "1E+6110"},
        {"description": "[decq668] fold-down full sequence", "input": "1E+6111"},
        {"description": "[decq072] Nmin and below", "input": "1E-6143"},
        {
          "description": "[decq076] Nmin and below",
          "input": "1.000000000000000000000000000000001E-6143"
        },
        {
          "description": "[decq036] fold-downs (more below)",
          "input": "1.230000000000000000000000000000000E+6144"
        },
        {"description": "[decq062] fold-downs (more below)", "input": "1.23"},
        {
          "description": "[decq034] Nmax and similar",
          "input": "1.234567890123456789012345678901234E+6144"
        },
        {"description": "[decq441] exponent lengths", "input": "7"},
        {"description": "[decq449] exponent lengths", "input": "7E+5999"},
        {"description": "[decq447] exponent lengths", "input": "7E+999"},
        {"description": "[decq445] exponent lengths", "input": "7E+99"},
        {"description": "[decq443] exponent lengths", "input": "7E+9"},
        {
          "description": "[decq842] VG testcase",
          "input": "7.049000000000010795488000000000000E-3097"
        },
        {"description": "[decq841] VG testcase", "input": "8.000000000000000000E-1550"},
        {"description": "[decq840] VG testcase", "input": "8.81125000000001349436E-1548"},
        {"description": "[decq701] Selected DPD codes", "input": "9"},
        {
          "description": "[decq032] Nmax and similar",
          "input": "9.999999999999999999999999999999999E+6144"
        },
        {"description": "[decq702] Selected DPD codes", "input": "10"},
        {"description": "[decq057] fold-downs (more below)", "input": "12"},
        {"description": "[decq703] Selected DPD codes", "input": "19"},
        {"description": "[decq704] Selected DPD codes", "input": "20"},
        {"description": "[decq705] Selected DPD codes", "input": "29"},
        {"description": "[decq706] Selected DPD codes", "input": "30"},
        {"description": "[decq707] Selected DPD codes", "input": "39"},
        {"description": "[decq708] Selected DPD codes", "input": "40"},
        {"description": "[decq709] Selected DPD codes", "input": "49"},
        {"description": "[decq710] Selected DPD codes", "input": "50"},
        {"description": "[decq711] Selected DPD codes", "input": "59"},
        {"description": "[decq712] Selected DPD codes", "input": "60"},
        {"description": "[decq713] Selected DPD codes", "input": "69"},
        {"description": "[decq714] Selected DPD codes", "input": "70"},
        {"description": "[decq715] Selected DPD codes", "input": "71"},
        {"description": "[decq716] Selected DPD codes", "input": "72"},
        {"description": "[decq717] Selected DPD codes", "input": "73"},
        {"description": "[decq718] Selected DPD codes", "input": "74"},
        {"description": "[decq719] Selected DPD codes", "input": "75"},
        {"description": "[decq720] Selected DPD codes", "input": "76"},
        {"description": "[decq721] Selected DPD codes", "input": "77"},
        {"description": "[decq722] Selected DPD codes", "input": "78"},
        {"description": "[decq723] Selected DPD codes", "input": "79"},
        {"description": "[decq056] fold-downs (more below)", "input": "123"},
        {"description": "[decq064] fold-downs (more below)", "input": "123.45"},
        {"description": "[decq732] Selected DPD codes", "input": "520"},
        {"description": "[decq733] Selected DPD codes", "input": "521"},
        {"description": "[decq740] DPD: one of each of the huffman groups", "input": "777"},
        {"description": "[decq741] DPD: one of each of the huffman groups", "input": "778"},
        {"description": "[decq742] DPD: one of each of the huffman groups", "input": "787"},
        {"description": "[decq746] DPD: one of each of the huffman groups", "input": "799"},
        {"description": "[decq743] DPD: one of each of the huffman groups", "input": "877"},
        {
          "description": "[decq753] DPD all-highs cases (includes the 24 redundant codes)",
          "input": "888"
        },
        {
          "description": "[decq754] DPD all-highs cases (includes the 24 redundant codes)",
          "input": "889"
        },
        {
          "description": "[decq760] DPD all-highs cases (includes the 24 redundant codes)",
          "input": "898"
        },
        {
          "description": "[decq764] DPD all-highs cases (includes the 24 redundant codes)",
          "input": "899"
        },
        {"description": "[decq745] DPD: one of each of the huffman groups", "input": "979"},
        {
          "description": "[decq770] DPD all-highs cases (includes the 24 redundant codes)",
          "input": "988"
        },
        {
          "description": "[decq774] DPD all-highs cases (includes the 24 redundant codes)",
          "input": "989"
        },
        {"description": "[decq730] Selected DPD codes", "input": "994"},
        {"description": "[decq731] Selected DPD codes", "input": "995"},
        {"description": "[decq744] DPD: one of each of the huffman groups", "input": "997"},
        {
          "description": "[decq780] DPD all-highs cases (includes the 24 redundant codes)",
          "input": "998"
        },
        {
          "description": "[decq787] DPD all-highs cases (includes the 24 redundant codes)",
          "input": "999"
        },
        {"description": "[decq053] fold-downs (more below)", "input": "1234"},
        {"description": "[decq052] fold-downs (more below)", "input": "12345"},
        {"description": "[decq792] Miscellaneous (testers' queries, etc.)", "input": "30000"},
        {"description": "[decq793] Miscellaneous (testers' queries, etc.)", "input": "890000"},
        {
          "description": "[decq824] values around [u]int32 edges (zeros done earlier)",
          "input": "2147483646"
        },
        {
          "description": "[decq825] values around [u]int32 edges (zeros done earlier)",
          "input": "2147483647"
        },
        {
          "description": "[decq826] values around [u]int32 edges (zeros done earlier)",
          "input": "2147483648"
        },
        {
          "description": "[decq827] values around [u]int32 edges (zeros done earlier)",
          "input": "2147483649"
        },
        {
          "description": "[decq828] values around [u]int32 edges (zeros done earlier)",
          "input": "4294967294"
        },
        {
          "description": "[decq829] values around [u]int32 edges (zeros done earlier)",
          "input": "4294967295"
        },
        {
          "description": "[decq830] values around [u]int32 edges (zeros done earlier)",
          "input": "4294967296"
        },
        {
          "description": "[decq831] values around [u]int32 edges (zeros done earlier)",
          "input": "4294967297"
        },
        {"description": "[decq022] Normality", "input": "1111111111111111111111111111111111"},
        {"description": "[decq020] Normality", "input": "1234567890123456789012345678901234"},
        {"description": "[decq550] Specials", "input": "9999999999999999999999999999999999"}
    ];

    data.forEach(function(testCase) {
        print(`Test - ${testCase.description}`);
        var output = NumberDecimal(testCase.input).toString();
        if (testCase.expected) {
            assert.eq(output, `NumberDecimal("${testCase.expected}")`);
        } else {
            assert.eq(output, `NumberDecimal("${testCase.input}")`);
        }
    });
}());