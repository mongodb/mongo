# JS Debugging in the MongoDB Shell


Sample JS Test:
```js
let x = 42;
let y = ["a", 15, [1, 2, 3]];
let z = {id: ObjectId(), value:42};

debugger;
assert.eq(x, 7);
assert.eq(q, "foo");
print("Test Passed!");
```

Running this test from the shell will fail since `x != 7`, and the `debugger` is a no-op (does not have any callback handler):
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

Should pause first at line 5, and prompt the user for input:
```
JSDEBUG> JavaScript execution paused in 'debugger' statement.
JSDEBUG> Type 'dbcont' to continue
JSDEBUG@jstests/my_test.js:5> 
```

Inspect variables:
```
JSDEBUG@jstests/my_test.js:5> x
42
JSDEBUG@jstests/my_test.js:5> y
[ "a", 15, [ 1, 2, 3 ] ]
JSDEBUG@jstests/my_test.js:5> z
{ "id" : ObjectId("698e1464cdc054c85288e998"), "value" : 42 }
```

Modify `x` so it passes its upcoming assertion:
```
JSDEBUG@jstests/my_test.js:5> x = 7
7
```

The `q` variable does not exist yet, so we can set it to pass its upcoming assertion:
```
JSDEBUG@jstests/my_test.js:5> q
ReferenceError: q is not defined
JSDEBUG@jstests/my_test.js:5> q = "foo"
foo
```

Send a `dbcont` command to continue execution, and now the test passes!
```
JSDEBUG@jstests/my_test.js:5> dbcont
JSDEBUG> Continuing execution...
Test Passed!
All pids dead / alive (0): 
Searching for files in: /home/ubuntu/mongo
```
