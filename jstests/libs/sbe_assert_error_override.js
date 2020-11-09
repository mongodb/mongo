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
    [28651, 5073201],
    [16006, 4997703, 4998202],
    [28689, 5126701],
    [28690, 5126702],
    [28691, 5126703],
    [16020, 5066300],
    [16007, 5066300],
    [16608, 4848401],
    [16609, 5073101],
    [16610, 4848403],
    [16555, 5073102],
    [28680, 4903701],
    [28689, 5126701],
    [28690, 5126702],
    [28691, 5126703],
    [28765, 4822870],
    [28714, 4903710],
    [28761, 4903708],
    [28765, 4903700, 4903702, 4903703, 4903704, 4903705, 4903707, 4903709],
    [28766, 4903706],
    [31034, 4848972],
    [31095, 4848972],
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
    [40485, 5075307, 4997704, 4998201],
    [40515, 4848979],
    [40517, 4848980, 4997701],
    [40521, 4997702],
    [40522, 4997700],
    [40523, 4848972],
    [40533, 4998200],
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
}());
