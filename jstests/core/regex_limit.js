/**
 * Test the behavior of very, very long regex patterns.
 */
(function() {
    "use strict";

    const coll = db.regex_limit;
    coll.drop();

    const kMaxRegexPatternLen = 32764;

    // Populate the collection with a document containing a very long string.
    assert.commandWorked(coll.insert({z: "c".repeat(100000)}));

    // Test that a regex exactly at the maximum allowable pattern length can find a document.
    const patternMaxLen = "c".repeat(kMaxRegexPatternLen);
    assert.eq(1, coll.find({z: {$regex: patternMaxLen}}).itcount());
    assert.eq(1, coll.find({z: {$in: [new RegExp(patternMaxLen)]}}).itcount());

    // Test that a regex pattern exceeding the limit fails.
    const patternTooLong = "c".repeat(kMaxRegexPatternLen + 1);
    assert.commandFailedWithCode(coll.runCommand("find", {filter: {z: {$regex: patternTooLong}}}),
                                 ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        coll.runCommand("find", {filter: {z: {$in: [new RegExp(patternTooLong)]}}}),
        ErrorCodes.BadValue);
}());
