# Mongo Shell JavaScript Libraries

## Assertion Library

The assertion library `assert.js` offers the general-purpose assertion function `assert()` and a
number of specialized assertion functions.

### General-Purpose Assert Function

The general-purpose `assert(b, msg, attr)` function expects a Boolean argument `b` and throws an
error if the argument evaluates to `false`. Otherwise, the function returns no value (`undefined`).

In case of a thrown error, the optional argument `msg` will be a part of the error message. `msg`
can be a function without any arguments. In this case, the result of `msg()` will be a part of the
thrown error message. This feature becomes practical in specialized assertion functions like
`assert.throws()`, `assert.soon()`, etc. where the first argument can also be a function.

Another optional argument `attr` of type 'object' allows attaching structured attributes to the
thrown error object. This is helpful when analyzing log entries created by failed assertions.

In case when `TestData.logFormat === "json"`, the failure of the assertion
`assert(false, "Invalid phase", {phase: config.phase})` creates the following log output (not
pretty-printed by default):

```
{
  "t": {
    "$date": "2025-02-25T13:33:40.569Z"
  },
  "logger": "js_test:hybrid_unique_index_with_updates",
  "s": "E",
  "c": "QUERY",
  "ctx": "js",
  "msg": "uncaught exception",
  "id": 10004100,
  "attr": {
    "errmsg": "uncaught exception: AssertionError: assert failed : Invalid phase",
    "code": 271,
    "codeName": "JavaScriptErrorWithStack",
    "originalError": {
      "code": 139,
      "codeName": "JSInterpreterFailure",
      "errmsg": "Error: assert failed : Invalid phase"
    },
    "stack": [
      "AssertionError@src/mongo/shell/assert.js:100:9",
      "_doassert@src/mongo/shell/assert.js:139:19",
      "assert@src/mongo/shell/assert.js:191:18",
      "runTest@jstests/noPassthrough/index_builds/hybrid_unique_index_with_updates.js:165:11",
      "@jstests/noPassthrough/index_builds/hybrid_unique_index_with_updates.js:177:8"
    ],
    "extra": {
      "phase": {
        "$undefined": true
      }
    }
  }
}
```

In the plain text mode (without `TestData.logFormat == "json"`), the exception is printed as
follows:

```
[js_test:hybrid_unique_index_with_updates] uncaught exception: Error: assert failed : Invalid phase { "phase" : undefined } :
[js_test:hybrid_unique_index_with_updates] doassert@src/mongo/shell/assert.js:20:14
[js_test:hybrid_unique_index_with_updates] _doassert@src/mongo/shell/assert.js:141:17
[js_test:hybrid_unique_index_with_updates] assert@src/mongo/shell/assert.js:191:18
[js_test:hybrid_unique_index_with_updates] runTest@jstests/noPassthrough/index_builds/hybrid_unique_index_with_updates.js:165:11
[js_test:hybrid_unique_index_with_updates] @jstests/noPassthrough/index_builds/hybrid_unique_index_with_updates.js:177:8
```

### Special-Purpose Assert Functions

Special-purpose assert functions help to reduce code duplication and provide more detailed default
assertion messages. For instance, `assert.eq(a, b)` is more useful than `assert(a === b)` as it will
print the values of both arguments, in case they are not equal.

The special-purpose assert functions also accept `msg` and `attr` arguments along with their own
specific arguments.

There are many special-purpose assert functions. The following list might be incomplete.

Comparisons:

- `assert.eq()`
- `assert.docEq()`
- `assert.setEq()`
- `assert.sameMembers()`
- `assert.fuzzySameMembers()`
- `assert.neq()`
- `assert.isnull()`
- `assert.lt()`
- `assert.lte()`
- `assert.gt()`
- `assert.gte()`
- `assert.between()`
- `assert.betweenIn()`
- `assert.betweenEx()`
- `assert.close()`
- `assert.closeWithinMS()`

Containment:

- `assert.hasFields()`
- `assert.contains()`
- `assert.doesNotContain()`
- `assert.containsPrefix()`
- `assert.includes()`

Assertions with retries and timeouts:

- `assert.soon()`
- `assert.soonNoExcept()`
- `assert.retry()`
- `assert.retryNoExcept()`
- `assert.time()`

Exceptions:

- `assert.throws()`
- `assert.throwsWithCode()`
- `assert.doesNotThrow()`

MongoDB commands:

- `assert.commandWorked()`
- `assert.commandFailed()`
- `assert.commandFailedWithCode()`
- `assert.writeOK()`
- `assert.writeError()`
- `assert.writeErrorWithCode()`
- `assert.noAPIParams()`

See more details on specific assert functions and their signatures in [assert.js](assert.js).

## Types Library

The types library `types.js` defines several new object types within the global scope:

- `BinData`
- `BSONAwareMap`
- `DBRef`
- `DBPointer`
- `NumberDecimal`
- `NumberInt`
- `NumberLong`
- `ObjectId`
- `Timestamp`

It also adds some functionality to already available (standard) object types `Array`, `Date`, `Map`,
`Number`, `Object`, `RegExp`, `Set`, and `String`.

### Value Serialization

The `types.js` library offers additional options for JavaScript value serialization.

The `tojson()` and `tojsononeline()` functions, as well as `tojson()` object methods serialize the
first argument (or `this` in case of object methods) to a string that can be used to deserialize it
with `eval()`. Additional arguments allow to modify the serialization format (pretty-printing and
indentation), the order of property names, and control the depth of serialization. The return value
is not always a valid JSON string. Therefore, it should not be used for logging.

The `toJsonForLog()` function serializes the first argument to a valid JSON string and is intended
for printing JavaScript values for logging and debugging. Like `tostrictjson()`, which serializes
BSON objects and arrays to the
[EJSON](https://www.mongodb.com/docs/manual/reference/mongodb-extended-json/) format that includes
type information, `toJsonForLog()` also accepts non-object types, recognizes recursive objects, and
provides more detailed serializations for commonly used JavaScript object classes like `Set`, `Map`,
`Error`, etc. Unlike `tojson()`, the result of `eval(toJsonForLog(x))` will not always evaluate into
an object equivalent to `x` and may throw a syntax error.

## Utils Library

The utilities library `utils.js` contains a wide range of utility functions and objects. For
instance:

- testing-related objects and functions `jsTest`, `jsTestName()`, `jsTestOptions`
- mongo shell essential functions `defaultPrompt()` and `help()`
- Geo functions
- replica set control functions
- logging functions

See more details on specific functions and their signatures in [utils.js](utils.js).

### Logging

Logging functions allow printing debug information depending on the chosen level of severity /
verbosity. The `utils.js` offers the following logging functions:

- `jsTestLog()`
- `jsTest.log()`
- `jsTest.log.error()`
- `jsTest.log.warning()`
- `jsTest.log.info()`
- `jsTest.log.debug()`.

All these functions expect the `msg` argument, which can be of any type, and an optional `attr`
argument, which must be of object type. Similar to the assertion library, the `attr` argument allows
separation of structured data from the text message to simplify log analysis (see also
[MongoDB Structured Logging](https://www.mongodb.com/docs/manual/reference/log-messages/#structured-logging)).

The `jsTestLog()` and `jsTest.log()` accept an optional severity level argument which defaults to
`info`. The logging format and severity level can be changed by modifying `logLevel` and `logFormat`
properties of the global `TestData` object.

In case when `TestData.logFormat === "json"`, the call of
`jsTest.log.info("running test with config", {config})` produces the following output:

```
{
    "t": {
        "$date": "2025-02-25T09:47:29.319+00:00"
    },
    "logger": "js_test:hybrid_unique_index_with_updates",
    "s": "I",
    "c": "js_test",
    "ctx": "hybrid_unique_index_with_updates",
    "msg": "running test with config",
    "attr": {
        "config": {
            "operation": "insert",
            "resolve": true,
            "phase": 0
        }
    }
}
```

Note, that the original log line is not pretty-printed for log size reduction and easier analysis.
Pretty-printing of interesting log lines can be done in post-processing with `jq` command-line tool
or on-demand in a log analysis tool such as [Parsley](https://parsley.mongodb.com/) or
[l2](https://github.com/10gen/l2/).

Extracting such log lines and pretty-printing them can be done with the following `jq` command line
(the `-c` option disables pretty-printing):

```
jq -c 'select(.msg == "running test with config")' <log file name>
```

In the plain text mode (without `TestData.logFormat == "json"`), the log entries are concatenated
with a space:

```
[js_test:hybrid_unique_index_with_updates] [jsTest] ----
[js_test:hybrid_unique_index_with_updates] [jsTest] running test with config {
[js_test:hybrid_unique_index_with_updates] [jsTest] 	"config" : {
[js_test:hybrid_unique_index_with_updates] [jsTest] 		"operation" : "insert",
[js_test:hybrid_unique_index_with_updates] [jsTest] 		"resolve" : true,
[js_test:hybrid_unique_index_with_updates] [jsTest] 		"phase" : 0
[js_test:hybrid_unique_index_with_updates] [jsTest] 	}
[js_test:hybrid_unique_index_with_updates] [jsTest] }
[js_test:hybrid_unique_index_with_updates] [jsTest] ----
```

## Connections

The shell provides different ways to connect to MongoDB deployments, handling single nodes, replica sets, and sharded clusters with multiple routers.

### Direct Connection (`Mongo` type)

The `Mongo` class represents a direct connection to a MongoDB deployment. This is the standard connection type used when connecting to:

- A single `mongod` instance.
- A Replica Set (when the connection string includes a `replicaSet` parameter).
- A single `mongos` instance.

The `Mongo` object manages the connection state, including authentication, read preferences, write concerns, and logical sessions. It is defined in `src/mongo/shell/mongo.js` (with core implementation in C++).

### Multiple Mongos Fixtures

When testing sharded clusters, it is often necessary to interact with multiple `mongos` to verify behavior across different routers.
The production workload (based on drivers specs) attempts to balance the workload based on the speed response of the ping command on every router.
Given in production is not possible to deterministically predict where the next command will be dispatched, we require a way to

- Hold a pool of direct connection against every mongos
- Dispach non-determinstically every command against the pool of connection
- Maintain the same constaints of the drivers.

#### Multi-Router Mongo

The `MultiRouterMongo` class (defined in `src/mongo/shell/multi_router.js`) wraps a pool of connections to multiple `mongos` instances. It acts as a proxy that routes commands:

- **Stateless Operations**: Dispatched to a random `mongos` in the pool to simulate non-deterministic load balancing.
- **Stateful Operations**:
  - **Transactions**: Pinned to a specific `mongos` for the duration of the transaction using `SessionMongoMap`.
  - **Cursors**: `getMore`, `releaseMemory` and `killCursors` commands are routed to the `mongos` that established the initial cursor via `find` or `aggregate`
- **Primary Fallback**: Maintains a "primary" connection for operations that require a stable reference or are not yet supported for random dispatch.

**Cluster Time**:
The clusterTime in a Multi-Router Mongo is a global single instance shared among every connection. While every connection will maintain its own clusterTime, the value will be ignored and any command will always carry the shared one, which is held by the primary connection.

#### Testing-Specific Deviations

While `MultiRouterMongo` primarily aims to mimic standard driver behavior (like random selection), certain operations deviate to accommodate the MongoDB testing framework's assumptions. In our testing environment, we often expect configuration changes to apply cluster-wide immediately, whereas a real driver would only affect the specific node it talks to.

- **Broadcasted Refreshes**: Commands that refresh the router's view of the cluster (e.g., `flushRouterConfig`) are broadcast to **all** connected mongos instances. This ensures that a test step intending to update the routing table does so for the entire test fixture, preventing stale routing errors on subsequent commands.

- **Broadcasted Server Parameters**: `setParameter` commands are node-specific by definition. However, many tests assume that setting a parameter (like a log level or feature flag) applies to the "system" they are testing. To support these tests without modification, `MultiRouterMongo` broadcasts `setParameter` to all routers in the pool.

### Connection Factory

The global `connect()` function is the primary entry point for establishing connections in the shell.

#### `connect(url, user, pass)`

The `connect` function parses the provided connection string and uses a `connectionFactory` to determine the appropriate connection object to return:

1. **Standard Connection**: If the URI contains a single host or specifies a Replica Set (`setName`), it returns a standard `Mongo` object.
2. **Multi-Router Connection**: If the URI lists multiple hosts and is _not_ a Replica Set, it attempts to create a `MultiRouterMongo` instance.
   - If the multi-router connection fails (e.g., some hosts are unreachable), it gracefully falls back to a standard `Mongo` connection using the first available host.

This factory pattern allows tests and scripts to use `connect()` agnostically, receiving a multi-router aware connection automatically when the environment supports it.

Important Note: We should always prefer the `connect` function to re-connect against the same end-point unless a specific behaviour is required.
Example:

```
// Connect the first time
const db = connect(uri);

// ... later in the test ...

// Reconnect against the same end-point (automatically handles multi-router or single connection)
const newConn = connect(db.getMongo().uri).getMongo();
```

This will ensure to return the correct connection type.
