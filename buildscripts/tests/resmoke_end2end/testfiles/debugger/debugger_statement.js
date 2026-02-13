let x = 42;
let y = ["a", 15, [1, 2, 3]];
let z = {"foo": [3, "bar"]};

// should be no-op without debug mode
debugger; // eslint-disable-line no-debugger

assert.eq(x, 42);
print("Test Passed!");
