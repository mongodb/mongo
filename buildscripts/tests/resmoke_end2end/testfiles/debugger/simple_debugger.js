// Test that the debugger statement is hit and variables can be inspected/modified
let x = 42;
let y = ["a", 15, [1, 2, 3]];

debugger; // eslint-disable-line no-debugger

// These assertions will fail unless debugger modifies the variables
assert.eq(x, 7);
assert.eq(y[1], 99);
print("Test Passed!");
