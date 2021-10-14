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
    [9, 5166503, 5166605, 5338802],
    [16006, 4997703, 4998202],
    [16007, 5066300],
    [16020, 5066300],
    [16554, 4974201, 4974203],
    [16555, 5073102],
    [16608, 4848401],
    [16609, 5073101],
    [16610, 4848403],
    [16611, 5154000],
    [16612, 4974202],
    [16702, 5073001],
    [17041, 5159200],
    [28651, 5073201],
    [28664, 5153400],
    [28680, 4903701],
    [28689, 5126701],
    [28690, 5126702],
    [28691, 5126703],
    [28714, 4903710],
    [28761, 4903708],
    [
        28765,
        4903700,
        4903702,
        4903703,
        4903704,
        4903705,
        4903707,
        4903709,
        4822870,
        4995501,
        4995502
    ],
    [28766, 4903706],
    [31034, 4848972],
    [31095, 4848972],
    [34435, 5154901],
    [34443, 5154300],
    [34444, 5154303],
    [34445, 5154301],
    [34446, 5154304],
    [34447, 5154302],
    [34448, 5154305],
    [34449, 5154306],
    [40066, 4934200],
    [40085, 5155402],
    [40086, 5155400],
    [40087, 5155401],
    [40091, 5075300],
    [40092, 5075301, 5075302],
    [40093, 5075300],
    [40094, 5075301, 5075302],
    [40096, 5075303, 5075305],
    [40097, 5075304, 5075306],
    [40485, 4997704, 4998201, 5075307, 5166505, 5166602],
    [40515, 4848979],
    [40517, 4848980, 4997701, 5166504, 5166601],
    [40521, 4997702],
    [40522, 4997700],
    [40523, 4848972],
    [40533, 4998200],
    [50989, 4995503],
    [51044, 5688500],
    [51045, 5688500],
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
    [51744, 5154400],
    [51745, 5154400],
    [51746, 5154400],
    [5166306, 5166502],
    [5166307, 5166500, 5166501],
    [5166403, 5166603],
    [5166404, 5166604],
    [5166405, 5166606],
    [5166502, 5439013],
    [5166503, 5439014],
    [5338800, 5338801],
    [5338801, 5439015],
    [5338802, 5439016],
    [5687301, 5687400],
    [5687302, 5687401],
    [292, 5859000],
    [6045000, 5166606]
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
