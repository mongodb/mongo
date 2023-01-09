/**
 * Test the behavior of very, very long regex patterns.
 */
(function() {
"use strict";

const coll = db.regex_limit;
coll.drop();

const kMaxRegexPatternLen = 16384;

// Populate the collection with a document containing a very long string.
assert.commandWorked(coll.insert({z: "c".repeat(100000)}));

function tryRegexLength(coll, n) {
    const re = "c".repeat(n);
    const res = coll.runCommand("find", {filter: {z: {$regex: re}}});
    if (res.ok == 1)
        return true;
    if (res.ok == 0 && res.code == 51091)
        return false;
    // Something else went wrong.
    assert.commandWorked(res);
}

// Returns the longest regex length the server will accept.
function probeMaxPatternLength(coll) {
    // Exponential search and bisect to find the exact failure point.
    // Half-open interval `range`: [lastKnownGood, firstKnownBad).
    let range = [1 << 0, 1 << 0];
    while (true) {
        if (!tryRegexLength(coll, range[1]))
            break;
        range[0] = range[1];
        range[1] *= 2;
    }
    while (true) {
        let mid = Math.floor((range[0] + range[1]) / 2);
        if (mid == range[0])
            break;
        if (tryRegexLength(coll, mid)) {
            range[0] = mid;
        } else {
            range[1] = mid;
        }
    }
    return range[0];
}

assert(probeMaxPatternLength(coll) >= kMaxRegexPatternLen);

// Test that a regex exactly at the maximum allowable pattern length can find a document.
const patternMaxLen = "c".repeat(kMaxRegexPatternLen);
assert.eq(1, coll.find({z: {$regex: patternMaxLen}}).itcount());
assert.eq(1, coll.find({z: {$in: [new RegExp(patternMaxLen)]}}).itcount());

// Test that a regex pattern exceeding the limit fails.
const patternTooLong = "c".repeat(2 * kMaxRegexPatternLen + 1);
assert.commandFailedWithCode(coll.runCommand("find", {filter: {z: {$regex: patternTooLong}}}),
                             51091);
assert.commandFailedWithCode(
    coll.runCommand("find", {filter: {z: {$in: [new RegExp(patternTooLong)]}}}), 51091);
}());
