/**
 * Test that the seed _srand() returns can be used to reproduce the values _rand() returns.
 */
(function() {
"use strict";

const seed1 = _srand();
const values1 = [_rand(), _rand(), _rand()];

const seed2 = _srand(seed1);
const values2 = [_rand(), _rand(), _rand()];

assert.eq(seed1, seed2);
assert.eq(values1, values2);
})();
