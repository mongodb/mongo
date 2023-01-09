
assert.eq(5, Array.sum([1, 4]), "A");
assert.eq(2.5, Array.avg([1, 4]), "B");

arr = [2, 4, 4, 4, 5, 5, 7, 9];
assert.eq(5, Array.avg(arr), "C");
assert.eq(2, Array.stdDev(arr), "D");
