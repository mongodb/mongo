# Log System Overview

The new log system adds capability to produce structured logs in the [Relaxed Extended JSON 2.0.0](https://github.com/mongodb/specifications/blob/master/source/extended-json.rst) format. Variables are logged as attributes where they are assigned a name. This is achieved with a new API which is inspired by [libfmt](https://fmt.dev/latest/index.html) and its Format API.

# Basic Usage

The log system is made available with the following header:

`#include "mongo/logv2/log.h"`

Logging is performed using function style macros:

`LOGV2(message-string, "name0"_attr = var0, ..., "nameN"_attr = varN);`

The message string contains the description of the log event with libfmt style replacement fields optionally embedded within it. The message string must comply with the [format syntax](https://fmt.dev/latest/syntax.html) from libfmt. 

Replacement fields are placed in the message string with curly braces `{}`. Everything not surrounded with curly braces is part of the message text. Curly brace characters can be output by escaping them using double braces: `{{` or `}}`. 

Attributes are created with the `_attr` user-defined string literal. The intermediate object that gets instantiated provides the assignment operator `=` for assigning a value to the attribute.

Attributes are associated with replacement fields in the message string by index. The first replacement field (from left to right) is associated with the first attribute and so forth. This order can be changed by providing an index as the *arg_id* in the replacement field ([grammar](https://fmt.dev/latest/syntax.html)). `{1}` would associate with attribute at index 1.

It is allowed to have more attributes than replacement fields in a log statement. However, having fewer attributes than replacement fields is not allowed.

Libfmt supports named replacement fields within the message string, this is not supported in the log system. However, *format spec* is supported but will only affect the human readable text output format.

Last, the message string must be a compile time constant. This is to be able to add compile time verification of log statements in the future.

#### Examples

```
LOGV2("Logging event, no replacement fields is OK");
```
```
const BSONObj& slowOperation = ...;
Milliseconds time = ...;
LOGV2("Operation {} is slow, took: {}", "op"_attr = slowOperation, "opTime"_attr = time);
```
```
LOGV2("Replication state change", "from"_attr = getOldState(), "to"_attr = getNewState());
```

### Log Component

Default log component is set using a per-cpp file macro (to be set before including `log.h`)

`#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault`

To override the default component, a separate logging API can be used that takes a `LogOptions` structure:

`LOGV2_OPTIONS(options, message-string, attr0, ..., attrN);`

`LogOptions` can be constructed with a `LogComponent` to avoid verbosity in the log statement.

#### Examples

```
LOGV2_OPTIONS({LogComponent::kCommand}, "Log event to specified component");
```

### Log Severity

`LOGV2` is the logging macro for the default informational (0) severity. To log to different severities there are separate logging macros to be used, they all take paramaters like `LOGV2`:

* `LOGV2_WARNING`
* `LOGV2_ERROR`
* `LOGV2_FATAL`

There is also variations that take `LogOptions` if needed:

* `LOGV2_WARNING_OPTIONS`
* `LOGV2_ERROR_OPTIONS`
* `LOGV2_FATAL_OPTIONS`

Fatal level log statements perform `fassert` after logging 

Debug-level logging is slightly different where an additional parameter (as integer) required to indicate the desired debug level:

`LOGV2_DEBUG(debug-level, message-string, attr0, ..., attrN);`

`LOGV2_DEBUG_OPTIONS(debug-level, options, message-string, attr0, ..., attrN);`

#### Examples

```
// Index specifier in replacement field
Status status = ...;
int remainingAttempts = ...;
LOGV2_ERROR("Initial sync failed. {1} attempts left. Reason: {0}", "reason"_attr = status, "remaining"_attr = remainingAttempts);
```

### Log Tags

Log tags are replacing the Tee from the old log system as the way to indicate that the log should also be written to a `RamLog` (accessible with the `getLog` command).

Tags are added to a log statement with the options API similarly to how non-default components are specified by constructing a `LogOptions`.

Multiple tags can be attached to a log statement using the bitwise or operator `|`.

#### Examples

```
LOGV2_WARNING_OPTIONS({LogTag::kStartupWarnings}, "XFS filesystem is recommended with WiredTiger");
```

# Type Support

### Built-in 

Many types basic types have built in support

* Boolean
* Integral types
  * Single char is logged as integer
* Enums
  * Logged as their underlying integral type
* Floating point types
  * long double is prohibited
* String types
  * std::string
  * StringData
  * char*
* BSON types
  * BSONObj
  * BSONArray
  * BSONElement
* BSON appendable types
  * `BSONObjBuilder::append` overload available
* boost::optional of any loggable type

### User defined types

To make a user defined type loggable it needs a serialization member function that the log system can bind to. The following list are function signatures the log system looks for in priority order:

1. `void serialize(BSONObjBuilder*) const`
2. `BSONObj toBSON() const`
3. `BSONArray toBSONArray() const`
4. `void serialize(fmt::memory_buffer&) const`
5. `std::string toString() const`

Stringification (**4** or **5**) is required to be able to produce human readable log output. Structured serialization (**1** to **3**) is optional but recommended to be able to produce structured log output.

*NOTE: `operator<<` is not used even if available*

### Container support

STL containers and data structures that have STL like interfaces are loggable as long as they contain loggable elements (built-in, user-defined or other containers).

#### Sequential containers

Sequential containers like `std::vector`, `std::deque` and `std::list` are loggable and the elements get formatted as JSON array in structured output.

#### Associative containers

Associative containers such as `std::map` and `stdx::unordered_map` loggable with the requirement that they key is of a string type. The structured format is a JSON object where the field names are the key.

#### Ranges

Ranges is loggable via helpers to indicate what type of range it is

* `seqLog(begin, end)`
* `mapLog(begin, end)`

##### Examples

```
std::array<int, 20> arrayOfInts = ...;
LOGV2("log container directly: {}", "values"_attr = arrayOfInts);
LOGV2("log iterator range: {}", "values"_attr = seqLog(arrayOfInts.begin(), arrayOfInts.end());
LOGV2("log first five elements: {}", "values"_attr = seqLog(arrayOfInts.data(), arrayOfInts.data() + 5);
``` 

```
StringMap<BSONObj> bsonMap = ...;
LOGV2("log map directly: {}", "values"_attr = bsonMap);
LOGV2("log map iterator range: {}", "values"_attr = mapLog(bsonMap.begin(), bsonMap.end());
``` 
