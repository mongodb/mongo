/**
 * Unit tests for the checksum_utils.js library.
 */
import {resultsetChecksum} from "jstests/query_golden/libs/checksum_utils.js";

// Checksum detects empty arrays
assert.neq(resultsetChecksum([]), resultsetChecksum([undefined]));

// Checksum detects empty dict values
assert.neq(resultsetChecksum({a: 1}), resultsetChecksum({a: undefined}));

// Checksum detects empty array dict values
assert.neq(resultsetChecksum({a: [1]}), resultsetChecksum({a: [undefined]}));

// Checksum detects changes in nested dicts
assert.neq(resultsetChecksum({a: {b: 1}}), resultsetChecksum({a: {b: 2}}));

// Checksum is array-order independent
assert.eq(resultsetChecksum([1, 2]), resultsetChecksum([2, 1]));

// Checksum is object-order independent
assert.eq(resultsetChecksum({a: 1, b: 2}), resultsetChecksum({b: 2, a: 1}));

// Checksum is row-order independent
assert.eq(resultsetChecksum([{a: 1}, {b: 2}]), resultsetChecksum([{b: 2}, {a: 1}]));

// Checksum is column-order independent
assert.eq(resultsetChecksum([{a: 1, b: 2}]), resultsetChecksum([{b: 2, a: 1}]));
