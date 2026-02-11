# JS Debugging in the MongoDB Shell


Sample JS Test:
```js
let x = 42;
let dbg = new Debugger(); // only found in Debug mode
assert.eq(x, 42);
print("Test Passed!");
```

Run this test from the shell:
```bash
./bazel-bin/install/bin/mongo --nodb jstests/my_test.js
```
Should error:
```
uncaught exception: ReferenceError: Debugger is not defined :
@jstests/my_test.js:2:11
failed to load: jstests/my_test.js
exiting with code -3
```

Run with the `--jsDebugMode` flag:
```bash
./bazel-bin/install/bin/mongo --nodb --jsDebugMode jstests/my_test.js
```

Should pass, with clues that the Debugger really is available (but more work to do):
```
uncaught exception: TypeError: debugger and debuggee must be in different compartments :
@(debugger-init):1:1
Test Passed!
```
