/**
 * Use _hashBSONElement to test that our hash function never changes. This test hard codes the hash
 * result for various inputs. For compatibility, these hardcoded values should never change over
 * time or across architectures. This is a good place to put tests for any edge cases in the hash
 * function that might be prone to change because of code changes or because of differences between
 * architectures.
 */
(function() {
'use strict';

const hashOfMaxNumberLong = NumberLong("1136124329541638701");
const hashOfLowestNumberLong = NumberLong("5744114172487291558");
const hashOfZeroNumberLong = NumberLong("5574369198691456941");

const hashTests = [
    // Hash value of a string.
    {key: "hashthis", expected: NumberLong("6271151123721111923")},

    // The smallest positive double that overflows a 64-bit signed int. This is a special case,
    // as described in SERVER-37183.
    {key: Math.pow(2, 63), expected: hashOfLowestNumberLong},

    // The next biggest number. Large doubles get clamped to the max 64-bit signed value before
    // being hashed.
    {key: Math.pow(2, 63) + Math.pow(2, 11), expected: hashOfMaxNumberLong},

    // Really large numbers and positive infinity also get clamped to the same value.
    {key: Math.pow(2, 500), expected: hashOfMaxNumberLong},
    {key: Infinity, expected: hashOfMaxNumberLong},

    // Just under the largest double that overflows a 64-bit signed int. This value gets
    // converted to a signed 64-bit int and then hashed.
    {key: Math.pow(2, 63) - Math.pow(2, 10), expected: NumberLong("-3954856262017896439")},

    // Lowest negative double that does not overflow a 64-bit signed int.
    {key: -Math.pow(2, 63), expected: hashOfLowestNumberLong},

    // Just above the lowest negative double that does not overflow a 64-bit signed int.
    {key: -(Math.pow(2, 63) - Math.pow(2, 10)), expected: NumberLong("-1762411739488908479")},

    // A negative overflowing double gets clamped to -2^63 before being hashed.
    {key: -(Math.pow(2, 63) + Math.pow(2, 11)), expected: hashOfLowestNumberLong},
    {key: -Infinity, expected: hashOfLowestNumberLong},

    // NaN values get converted to 0 and then hashed.
    {key: 0, expected: hashOfZeroNumberLong},
    {key: NumberLong("0"), expected: hashOfZeroNumberLong},
    {key: NaN, expected: hashOfZeroNumberLong},
    {key: -NaN, expected: hashOfZeroNumberLong},

    // Hash an object.
    {key: {a: 1, b: 2}, expected: NumberLong("-7076810813311352857")},

    // Hash an object with some corner-case values.
    {key: {a: Math.pow(2, 63), b: NaN}, expected: NumberLong("1223292051903137684")},
];

hashTests.forEach(test => {
    const hashResult = db.runCommand({_hashBSONElement: test.key, seed: 1});
    assert.commandWorked(hashResult);
    assert.eq(test.expected, hashResult.out, tojson(test.key));
});
})();
