/**
 * @tags: [
 *   assumes_read_concern_local,
 *   # Retryable writes are required for the remove() command in the test.
 *   requires_non_retryable_writes,
 * ]
 */
const kCollName = jsTestName();
const kMaxOldRegexPatternLength = 16384;

const buildAnchoredPattern = (length) => "^" + "y".repeat(length - 1);

const coll = db[kCollName];
coll.drop();

// Assume that the following queries succeed, despite long patterns being used.
[0, 1, 100, 1000, 10000].forEach((length) => {
    coll.remove({});
    assert.commandWorked(coll.insert({a: "y".repeat(kMaxOldRegexPatternLength + length)}));
    assert.eq(1, coll.find({a: {$regex: buildAnchoredPattern(kMaxOldRegexPatternLength + length)}}).itcount());
});

// Using a too long pattern will always fail.
let error = assert.throws(() => coll.find({a: {$regex: buildAnchoredPattern(32767)}}).itcount());
assert.commandFailedWithCode(error, 51091);
assert(
    error.message.includes(
        "Regular expression is invalid: pattern string is longer than the limit set by the application",
    ),
);
