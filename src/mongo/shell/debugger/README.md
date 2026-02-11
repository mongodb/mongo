# JS Debugging in the MongoDB Shell


Sample JS Test:
```js
let x = 42;
debugger;
assert.eq(x, 42);
print("Test Passed!");
```

Running this test from the shell will pass; the `debugger` is a no-op (does not have any callback handler):
```bash
./bazel-bin/install/bin/mongo --nodb jstests/my_test.js
```
Output:
```
Test Passed!
```

Run with the `--jsDebugMode` flag:
```bash
./bazel-bin/install/bin/mongo --nodb --jsDebugMode jstests/my_test.js
```

Should pass, with clues that a callback fired via the `debugger` statement:
```
[WIP] in debugger callback!
Test Passed!
```
