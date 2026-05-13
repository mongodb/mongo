## JavaScript Test Code Conventions

### Assertion Library

ALWAYS use our own assertion library that is defined at `src/mongo/shell/assert.js` and
automatically loaded into the global scope before running each test.

ALWAYS wrap commands with `assert.commandWorked()`, unless failures are expected and checked. Use
`assert.writeOK()` when working with legacy bulk write APIs.

Use `assert.soon()` only for asynchronous or eventually-consistent conditions.

### Mocha Style for New Test Files

Use the mocha style to structure test cases in newly added test files. The
`jstests/libs/mochalite.js` module implements `describe`, `it`, `before`, `beforeEach`, `afterEach`,
and `after`. Import only what you need. Feel free to add other mocha helper functions to
`jstests/libs/mochalite.js` if they would make the current test cleaner.

```js
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("feature under test", function () {
  before(function () {
    /* one-time setup */
  });
  it("does X", function () {
    /* ... */
  });
});
```

### Use JSON-Conformant Object Serialization in Logs

Pass objects to the `attr` parameter of assertion and logging functions instead of concatenating
`tojson()` into the message string. Most `assert.*` functions accept an `attr` object as their last
argument.

```js
// Bad — tojson() in the message string
assert(cursor.hasOwnProperty("metrics"), "metrics missing: " + tojson(cursor));

// Good — object passed via attr
assert(cursor.hasOwnProperty("metrics"), "metrics missing", {cursor});
```

For logging:

```js
// Bad
jsTest.log.info("got cursor: " + tojson(cursor));

// Good
jsTest.log.info("got cursor", {cursor});
```

### Resource and Settings Cleanup

ALWAYS clean up resources and settings immediately after they are no longer needed. Examples of
server resources are cursors and collections. Examples of settings are server parameters and test
framework settings stored in the global `TestData` object.
