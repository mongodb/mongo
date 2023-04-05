/**
 * For a number of errors, the engine will raise different error codes when SBE mode is enabled vs.
 * disabled. When SBE mode is enabled, these differences in error codes can cause tests to fail that
 * otherwise would have passed.
 *
 * To expedite efforts to enable more tests under SBE mode, this file provides overrides for the
 * assert.commandFailedWithCode() and assert.writeErrorWithCode() APIs so that they treat certain
 * groups of error codes as being equivalent to each other. Note that these overrides also affect
 * how assertError(), assertErrorCode(), and assertErrCodeAndErrMsgContains() behave.
 *
 * Below the 'equivalentErrorCodesList' variable contains all known groups of error codes that
 * should be treated as equivalent to each other. As new groups of equivalent error codes are
 * discovered, they should be added to the list below.
 *
 * Note: This file should _only_ be included in a test if it has been observed that the test fails
 * due to differences in error codes when SBE mode is enabled.
 */
(function() {
"use strict";

// Below is the list of known equivalent error code groups. As new groups of equivalent error codes
// are discovered, they should be added to this list.
const equivalentErrorCodesList = [
    [9, 5166503, 5166605, 5338802, 7157906, 7157924, 7157926],
    [16006, 4997703, 4997901, 4998202, 7157910, 7157915, 5157904],
    [16007, 5066300, 7158200],
    [16020, 5066300, 7158200],
    [16554, ErrorCodes.TypeMismatch, 4974201, 4974203, 7157723],
    [16555, 5073102, 7157721],
    [16608, 4848401],
    [16609, 5073101, 7157719],
    [16610, 4848403],
    [16611, 5154000, 7157718],
    [16612, 4974202, 7157722],
    [16702, 5073001, 7158201],
    [17041, 5159200, 7158300],
    [18533, 4997902, 4997903],
    [28651, 5073201, 7158305],
    [28664, 5153400, 7158000],
    [28680, 4903701, 7157701],
    [28689, 5126701],
    [28690, 5126702],
    [28691, 5126703],
    [28714, 4903710, 7157710],
    [28761, 4903708, 7157708],
    [
        28765,   4903700, 4903702, 4903703, 4903704, 4903705, 4903707, 4903709, 4822870, 4995501,
        4995502, 7157700, 7157702, 7157703, 7157704, 7157705, 7157707, 7157709, 7157800, 7157802
    ],
    [28766, 4903706, 7157706],
    [31034, 4848972, 7157916],
    [31095, 4848972, 7157916],
    [34435, 5154901, 7158002],
    [34443, 5154300, 7157711],
    [34444, 5154303, 7157714],
    [34445, 5154301, 7157712],
    [34446, 5154304, 7157715],
    [34447, 5154302, 7157713],
    [34448, 5154305, 7157716],
    [34449, 5154306, 7157717],
    [40066, 4934200, 7158303],
    [40085, 5155402, 7158202],
    [40086, 5155400, 7158203],
    [40087, 5155401, 7158204],
    [40091, 5075300, 7158007],
    [40092, 5075301, 5075302, 7158008, 7158009],
    [40093, 5075300, 7158007],
    [40094, 5075301, 5075302, 7158008, 7158009],
    [40096, 5075303, 5075305, 7158003, 7158005],
    [40097, 5075304, 5075306, 7158004, 7158006],
    [
        40485,
        4997704,
        4997906,
        4998201,
        5075307,
        5166505,
        5166602,
        7157903,
        7157909,
        7157913,
        7157920,
        5157903,
        5157901
    ],
    [40515, 4848979, 7157917],
    [40517, 4848980, 4997701, 4997905, 5166504, 5166601, 7157902, 7157912, 7157918, 7157919],
    [40521, 4997702, 7157914],
    [40522, 4997700, 7157911],
    [40523, 4848972, 7157916],
    [40533, 4998200, 7157908, 5157902, 5157900],
    [40684, 4997802, 4997803],
    [50989, 4995503, 7157803],
    [51044, 5688500, 7157801],
    [51045, 5688500, 7157801],
    [51104, 5073401],
    [51105, 5073405, 5126601],
    [51106, 5126603, 5126607],
    [51107, 5073406, 5126605],
    [51108, 5073401],
    [51109, 5073407, 5126602],
    [51110, 5073408, 5126604],
    [51111, 5073402],
    [51144, 5611301],
    [51151, 5126606],
    [51156, 5073403],
    [51246, 5291401],
    [51247, 5291402],
    [51744, 5154400, 7158302],
    [51745, 5154400, 7158302],
    [51746, 5154400, 7158302],
    [5166306, 5166502, 7157923],
    [5166307, 5166500, 5166501, 7157921, 7157922],
    [5166403, 5166603, 7157904],
    [5166404, 5166604, 5439013, 7157905],
    [5166405, 5166606, 7157907],
    [5166502, 5439013, 7157923],
    [5166503, 7157924],
    [5338800, 5338801, 7157925],
    [5338801, 5439015, 7157925],
    [5338802, 5439016, 7157926],
    [5687301, 5687400, 7157900],
    [5687302, 5687401, 7157901],
    [292, 5859000, 5843600, 5843601],
    [6045000, 5166606, 7157907],
    [146, 13548],
    [ErrorCodes.TypeMismatch, 5156200, 5156201, 7157720],
    [5439100, 40517, 7003907, 7157928, 7157930],
    [5439101, 40485, 7007908, 7157929, 7157931],
    [5439102, 5439012, 7157932],
    [5439103, 5439013, 7003902, 7157933, 7157935],
    [5439104, 9, 7003903, 7157934, 7157936],
    [5439105, 5439017, 7003904, 7003905, 7157937, 7157938, 7157940],
    [5439105, 5439018, 7003906, 7157939, 7157940],
    [5439106, 5439015, 7003909, 7157941, 7157943],
    [5439107, 5439016, 7003910, 7157942, 7157944],
    [17042, 5126900, 7158100],
    [17043, 5126900, 7158100],
    [17044, 5126900, 7158100],
    [17046, 5126900, 7158100],
    [17047, 5126900, 7158100],
    [17048, 5126900, 7158100],
    [17049, 5126900, 7158100],
    [51081, 5155300],
    [51080, 5155302],
    [31109, 5155301],
    [51082, 5155301],
    [51083, 5155301],
    [16004, 5155301],
    [40386, 5153200],
    [40398, 5153201],
    [40396, 5153202],
    [40397, 5153203, 5153205, 5153206],
    [40395, 5153204],
    [4940400, 5153207],
    [40391, 5153208],
    [40392, 5153209, 5153210, 5153211],
    [40393, 5153212],
    [40394, 5153213],
    [4940401, 5153214],
    [40390, 5153215]
];

// This map is generated based on the contents of 'equivalentErrorCodesList'. This map should _not_
// be modified. If you need to change which error codes are considered equivalent to each other, you
// should modify 'equivalentErrorCodesList' above.
const equivalentErrorCodesMap = function() {
    let mapOfSets = {};
    for (const arr of equivalentErrorCodesList) {
        for (const errorCode1 of arr) {
            if (!mapOfSets.hasOwnProperty(errorCode1)) {
                mapOfSets[errorCode1] = new Set();
            }

            for (const errorCode2 of arr) {
                if (errorCode1 != errorCode2) {
                    mapOfSets[errorCode1].add(errorCode2);
                }
            }
        }
    }

    let mapOfLists = {};
    for (const errorCode1 in mapOfSets) {
        let arr = [];
        for (const errorCode2 of mapOfSets[errorCode1]) {
            arr.push(errorCode2);
        }
        mapOfLists[errorCode1] = arr;
    }

    return mapOfLists;
}();

const lookupEquivalentErrorCodes = function(errorCodes) {
    if (errorCodes === assert._kAnyErrorCode) {
        return errorCodes;
    }

    if (!Array.isArray(errorCodes)) {
        errorCodes = [errorCodes];
    }

    let result = [];
    for (const errorCode1 of errorCodes) {
        result.push(errorCode1);
        if (equivalentErrorCodesMap.hasOwnProperty(errorCode1)) {
            for (const errorCode2 of equivalentErrorCodesMap[errorCode1]) {
                result.push(errorCode2);
            }
        }
    }

    return result;
};

// Override the assert.commandFailedWithCode() function.
const assertCommandFailedWithCodeOriginal = assert.commandFailedWithCode;
assert.commandFailedWithCode = function(res, expectedCode, msg) {
    return assertCommandFailedWithCodeOriginal(res, lookupEquivalentErrorCodes(expectedCode), msg);
};

// Override the assert.writeErrorWithCode() function.
const assertWriteErrorWithCodeOriginal = assert.writeErrorWithCode;
assert.writeErrorWithCode = function(res, expectedCode, msg) {
    return assertWriteErrorWithCodeOriginal(res, lookupEquivalentErrorCodes(expectedCode), msg);
};

// Override the assert.throwsWithCode() function.
const assertThrowsWithCodeOriginal = assert.throwsWithCode;
assert.throwsWithCode = function(func, expectedCode, params, msg) {
    return assertThrowsWithCodeOriginal(
        func, lookupEquivalentErrorCodes(expectedCode), params, msg);
};

// NOTE: Consider using 'assert.commandFailedWithCode' and 'assert.writeErrorWithCode' instead.
// This function should be only used when error code does not come from command. For instance, when
// validating explain output, which includes error code in the execution stats.
assert.errorCodeEq = function(actualErrorCode, expectedErrorCodes, msg) {
    if (expectedErrorCodes === assert._kAnyErrorCode) {
        return;
    }

    const equivalentErrorCodes = lookupEquivalentErrorCodes(expectedErrorCodes);
    assert(equivalentErrorCodes.includes(actualErrorCode),
           actualErrorCode + " not found in the list of expected error codes: " +
               tojson(equivalentErrorCodes) + ": " + msg);
};
}());
