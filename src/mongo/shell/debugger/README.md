# JS Debugging in the MongoDB Shell

> The [**VSCode Extension**](./vscode/README.md) provides interactive functionality using the VSCode
> UI.
>
> The remainder documents the supported `debugger` statement callbacks.

Use the `--jsdbg` flag for resmoke (or the `--jsDebugMode` flag directly on the mongo shell) to
trigger an interactive debug prompt when `debugger` statements are hit in JS test code.

Sample JS Test:

```js
let x = 42;
let y = ["a", 15, [1, 2, 3]];
let z = {id: ObjectId(), value: 42};

debugger;
assert.eq(x, 7);
assert.eq(q, "foo");
print("Test Passed!");
```

Running this test will fail since `x != 7`, and the `debugger` is a no-op (does not have any
callback handler):

```bash
buildscripts/resmoke.py run --suites=no_passthrough jstests/my_test.js
```

Output:

```
[js_test:my_test] Test Passed!
```

Run with the `--jsdbg` flag:

```bash
buildscripts/resmoke.py run --suites=no_passthrough --jsdbg jstests/my_test.js
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
[js_test:my_test] Test Passed!
```

## Architecture

- `debugger.cpp` is the main shell logic to invoke/wait in response to breakpoints and the UI. It
  interacts with the SpiderMonkey Debugger API.
- `adapter.cpp` is the Debug Adapter Protocol (DAP) message handler and TCP client. This connects to
  the VSCode extension, specifically `./vscode/session.js`.
  - This should _not_ handle any BSON/JSON directly, and only interface with the protocol.
- `protocol.cpp` is the relevant implementation of the
  [DAP specification](https://microsoft.github.io/debug-adapter-protocol//specification.html).
  - This should stand on its own _without_ the adapter, and encapsulate all BSON/JSON manipulation.

The VSCode client code is in `./vscode`, see more in its [README.md](./vscode/README.md).
