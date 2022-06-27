/**
 * Tests to validate limits for $regexFind, $regexFindAll and $regexMatch aggregation expressions.
 */
(function() {
'use strict';

load("jstests/aggregation/extras/utils.js");        // For assertErrorCode().
load('jstests/libs/sbe_assert_error_override.js');  // Override error-code-checking APIs.

const coll = db.regex_expr_limit;
coll.drop();
assert.commandWorked(coll.insert({z: "c".repeat(25000) + "d".repeat(25000) + "e"}));

function testRegexAgg(inputObj, expectedOutputForFindAll) {
    const resultFindAll =
        coll.aggregate([{"$project": {_id: 0, "matches": {"$regexFindAll": inputObj}}}]).toArray();
    assert.eq(resultFindAll, [{"matches": expectedOutputForFindAll}]);

    const resultFind =
        coll.aggregate([{"$project": {_id: 0, "matches": {"$regexFind": inputObj}}}]).toArray();
    assert.eq(
        resultFind,
        [{"matches": expectedOutputForFindAll.length == 0 ? null : expectedOutputForFindAll[0]}]);

    const resultMatch =
        coll.aggregate([{"$project": {_id: 0, "matches": {"$regexMatch": inputObj}}}]).toArray();
    assert.eq(resultMatch, [{"matches": expectedOutputForFindAll.length != 0}]);
}

function testRegexAggException(inputObj, exceptionCode, expression) {
    // If expression is defined, run tests only against that expression.
    if (expression != undefined) {
        assertErrorCode(coll, [{"$project": {"matches": {[expression]: inputObj}}}], exceptionCode);
        return;
    }
    assertErrorCode(coll, [{"$project": {"matches": {"$regexFindAll": inputObj}}}], exceptionCode);
    assertErrorCode(coll, [{"$project": {"matches": {"$regexFind": inputObj}}}], exceptionCode);
    assertErrorCode(coll, [{"$project": {"matches": {"$regexMatch": inputObj}}}], exceptionCode);
}

(function testLongRegex() {
    // Our limit on regex pattern length is 2^14.
    const kMaxRegexPatternLen = 16384;
    const patternMaxLen = "c".repeat(kMaxRegexPatternLen);

    // Test that a regex with maximum allowable pattern length can find a document.
    testRegexAgg({input: "$z", regex: patternMaxLen},
                 [{match: patternMaxLen, "idx": 0, "captures": []}]);

    // Test that a regex pattern exceeding the limit fails.
    const patternTooLong = patternMaxLen + "c";
    testRegexAggException({input: "$z", regex: patternTooLong}, 51111);
})();

(function testBufferOverflow() {
    // $regexFindAll will match each character individually, when the pattern is empty. If there
    // are 'n' characters in the input, it would result to 'n' individual matches. If the
    // pattern further has 'k' capture groups, then the output document will have 'n * k'
    // sub-strings representing the captures.
    const pattern = "(".repeat(150) + ")".repeat(150);
    // If the intermediate document size exceeds 64MB at any point, we will stop further
    // evaluation and throw an error.
    testRegexAggException({input: "$z", regex: pattern}, 51151, "$regexFindAll");

    const pattern2 = "()".repeat(150);
    testRegexAggException({input: "$z", regex: pattern2}, 51151, "$regexFindAll");
})();

(function testNumberOfCaptureGroupLimit() {
    const allowedCaptureGroups = 250;
    let pattern = "(d)".repeat(allowedCaptureGroups) + "e";
    const expectedOutputCaptures = new Array(allowedCaptureGroups).fill('d');

    testRegexAgg({input: "$z", regex: pattern}, [{
                     match: "d".repeat(allowedCaptureGroups) + "e",
                     "idx": 49750,
                     "captures": expectedOutputCaptures
                 }]);
})();

(function testMaxCaptureDepth() {
    const kMaxCaptureDepthLen = 250;
    // Create a pattern with 250 depth captures of the format '(((((...e...))))'.
    const patternMaxDepth = "(".repeat(kMaxCaptureDepthLen) + "e" +
        ")".repeat(kMaxCaptureDepthLen);
    const expectedOutputCaptures = new Array(kMaxCaptureDepthLen).fill('e');

    // Test that there is a match.
    testRegexAgg({input: "$z", regex: patternMaxDepth},
                 [{match: "e", "idx": 50000, "captures": expectedOutputCaptures}]);

    // Add one more and verify that regex expression throws an error.
    const patternTooLong = '(' + patternMaxDepth + ')';
    testRegexAggException({input: "$z", regex: patternTooLong}, 51111);
})();
})();
