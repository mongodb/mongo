# JS Debugging in the MongoDB Shell


Sample JS Test:
```js
let x = 42;
debugger;
assert.eq(x, 42);
debugger;
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

Should pause first at line 2, and prompt the user for input:
```
JSDEBUG> JavaScript execution paused in 'debugger' statement.
JSDEBUG> Type 'dbcont' to continue
JSDEBUG@jstests/my_test.js:2> 
```

Explicitly sending a `dbcont` command continues execution, until it hits the next `debugger` statement at line 4:
```
JSDEBUG@jstests/my_test.js:2> dbcont
JSDEBUG> Continuing execution...

JSDEBUG> JavaScript execution paused in 'debugger' statement.
JSDEBUG> Type 'dbcont' to continue
JSDEBUG@jstests/my_test.js:4> 
```

Continuing again resumes execution to finish the test:
```
JSDEBUG@jstests/my_test.js:4> dbcont
JSDEBUG> Continuing execution...
Test Passed!
```
