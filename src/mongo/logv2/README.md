# Log System Overview

The new log system adds capability to produce structured logs in the [Relaxed Extended JSON 2.0.0](https://github.com/mongodb/specifications/blob/master/source/extended-json.rst) format. The new API requires names to be given to variables, forming field names for the variables in structured JSON logs. Named variables are called attributes in the log system. Human readable log messages are built with a [libfmt](https://fmt.dev/6.1.1/index.html) inspired API, where attributes are inserted using replacement fields instead of being streamed together using the streaming operator `<<`.

# Basic Usage

The log system is made available with the following header:

`#include "mongo/logv2/log.h"`

To be able to include it a default log component needs to be defined in the cpp file before including `log.h`:

`#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault`

Logging is performed using function style macros:

`LOGV2(ID, message-string, "name0"_attr = var0, ..., "nameN"_attr = varN);`

The ID is a signed 32bit integer in the same number space as the error code numbers. It is used to uniquely identify a log statement. If changing existing code, using a new ID is strongly advised to avoid any parsing ambiguity. 

The message string contains the description of the log event with libfmt style replacement fields optionally embedded within it. The message string must comply with the [format syntax](https://fmt.dev/6.1.1/syntax.html#formatspec) from libfmt. 

Replacement fields are placed in the message string with curly braces `{}`. Everything not surrounded with curly braces is part of the message text. Curly brace characters can be output by escaping them using double braces: `{{` or `}}`. 

Attributes are created with the `_attr` user-defined literal. The intermediate object that gets instantiated provides the assignment operator `=` for assigning a value to the attribute.

Attributes are associated with replacement fields in the message string by name or index, using names is strongly recommended. When using unnamed replacement fields, attributes map to replacement fields in the order they appear in the message string. 

It is allowed to have more attributes than replacement fields in a log statement. However, having fewer attributes than replacement fields is not allowed.

The message string must be a compile time constant. This is to be able to add compile time verification of log statements in the future.

##### Examples

```
LOGV2(1000, "Logging event, no replacement fields is OK");
```
```
const BSONObj& slowOperation = ...;
Milliseconds time = ...;
LOGV2(1001, "Operation {op} is slow, took: {duration}", "op"_attr = slowOperation, "duration"_attr = time);
```
```
LOGV2(1002, "Replication state change", "from"_attr = getOldState(), "to"_attr = getNewState());
```

### Log Component

To override the default component, a separate logging API can be used that takes a `LogOptions` structure:

`LOGV2_OPTIONS(options, message-string, attr0, ..., attrN);`

`LogOptions` can be constructed with a `LogComponent` to avoid verbosity in the log statement.

##### Examples

```
LOGV2_OPTIONS(1003, {LogComponent::kCommand}, "Log event to specified component");
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

Fatal level log statements perform `fassert` after logging, using the provided ID as assert id. 

Debug-level logging is slightly different where an additional parameter (as integer) required to indicate the desired debug level:

`LOGV2_DEBUG(ID, debug-level, message-string, attr0, ..., attrN);`

`LOGV2_DEBUG_OPTIONS(ID, debug-level, options, message-string, attr0, ..., attrN);`

##### Examples

```
Status status = ...;
int remainingAttempts = ...;
LOGV2_ERROR(1004, "Initial sync failed. {remaining} attempts left. Reason: {reason}", "reason"_attr = status, "remaining"_attr = remainingAttempts);
```

### Log Tags

Log tags are replacing the Tee from the old log system as the way to indicate that the log should also be written to a `RamLog` (accessible with the `getLog` command).

Tags are added to a log statement with the options API similarly to how non-default components are specified by constructing a `LogOptions`.

Multiple tags can be attached to a log statement using the bitwise or operator `|`.

##### Examples

```
LOGV2_WARNING_OPTIONS(1005, {LogTag::kStartupWarnings}, "XFS filesystem is recommended with WiredTiger");
```

### Dynamic attributes

Sometimes there is a need to add attributes depending on runtime conditionals. To support this there is the `DynamicAttributes` class that has an `add` method to add named attributes one by one. This class is meant to be used when you have this specific requirement and is not the general logging API.

When finished, it is logged using the regular logging API but the `DynamicAttributes` instance is passed as the first attribute parameter. Mixing `_attr` literals with the `DynamicAttributes` is not supported.

When using the `DynamicAttributes` you need to be careful about parameter lifetimes. The `DynamicAttributes` binds attributes *by reference* and the reference must be valid when passing the `DynamicAttributes` to the log statement.

##### Examples

```
DynamicAttributes attrs;
attrs.add("str"_sd, "StringData value"_sd);
if (condition) {
    // getExtraInfo() returns a reference that is valid until the LOGV2 call below.
    // Be careful of functions returning by value
    attrs.add("extra"_sd, getExtraInfo());
}
LOGV2(1030, "dynamic attributes", attrs);
```

# Type Support

### Built-in 

Many basic types have built in support:

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
  * const char*
* Duration types
  * Special formatting, see below
* BSON types
  * BSONObj
  * BSONArray
  * BSONElement
* BSON appendable types
  * `BSONObjBuilder::append` overload available
* boost::optional of any loggable type

### User defined types

To make a user defined type loggable it needs a serialization member function that the log system can bind to. At a minimum a type needs a stringification function. This would be used to produce text output and used as a fallback for JSON output.

In order to offer more its string representation in JSON, a type would need to supply a  structured serialization function.

The system will bind a stringification function and optionally a structured serialization function. The system binds to the serialization functions by looking for functions in the following priority order:

##### Structured serialization function signatures

Member functions:

1. `void serialize(BSONObjBuilder*) const`
2. `BSONObj toBSON() const`
3. `BSONArray toBSONArray() const`

Non-member functions:

4. `toBSON(const T& val)` (non-member function)

##### Stringification function signatures

Member functions:

1. `void serialize(fmt::memory_buffer&) const`
2. `std::string toString() const`

Non-member functions:

3. `toString(const T& val)` (non-member function) 

Enums will only try to bind a `toString(const T& val)` non-member function. If one is not available the enum value will be logged as its underlying integral type.

*NOTE: No `operator<<` overload is used even if available*

##### Examples

```
class UserDefinedType {
public:
    void serialize(BSONObjBuilder* builder) const {
        builder->append("str"_sd, _str);
        builder->append("int"_sd, _int);
    }

    void serialize(fmt::memory_buffer& buffer) const {
        fmt::format_to(buffer, "UserDefinedType: (str: {}, int: {})", _str, _int);
    }

private:
    std::string _str;
    int32_t _int;
};
```

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

seqLog indicates that it is a sequential range where the iterators point to loggable value directly.

mapLog indicates that it is a range coming from an associative container where the iterators point to a key-value pair.


##### Examples

```
std::array<int, 20> arrayOfInts = ...;
LOGV2(1010, "log container directly: {values}", "values"_attr = arrayOfInts);
LOGV2(1011, "log iterator range: {values}", "values"_attr = seqLog(arrayOfInts.begin(), arrayOfInts.end());
LOGV2(1012, "log first five elements: {values}", "values"_attr = seqLog(arrayOfInts.data(), arrayOfInts.data() + 5);
``` 

```
StringMap<BSONObj> bsonMap = ...;
LOGV2(1013, "log map directly: {values}", "values"_attr = bsonMap);
LOGV2(1014, "log map iterator range: {values}", "values"_attr = mapLog(bsonMap.begin(), bsonMap.end());
``` 

### Duration types

Duration types have special formatting to match existing practices in the server code base. Their resulting format depends on the context they are logged.

When durations are formatted as JSON or BSON a unit suffix is added to the attribute name when building the field name. The value will be count of the duration as a number.

When logging containers with durations there is no attribute per duration instance that can have the suffix added. In this case durations are instead formatted as a BSON object.

##### Examples

`"duration"_attr = Milliseconds(10)`

Text | JSON/BSON
---- | ---------
`10 ms` | `"durationMillis": 10`

```
std::vector<Nanoseconds> nanos = {Nanoseconds(200), Nanoseconds(400)};
"samples"_attr = nanos
```

Text | JSON/BSON
---- | ---------
`(200 ns, 400 ns)` | `"samples": [{"durationNanos": 200}, {"durationNanos": 400}]`

# Output formats

Desired log output format is set to mongod, mongos and the mongo shell using the `-logFormat` option. Available values are `text` and `json`. Currently text formatting is default.

## Text

Produces legacy log statements matching the old log system. This format may or may not be removed for the 4.4 server release. 

## JSON

Produces structured logs of the [Relaxed Extended JSON 2.0.0](https://github.com/mongodb/specifications/blob/master/source/extended-json.rst) format. Below is an example of a log statement in C++ and a pretty-printed JSON output:

```
BSONObjBuilder builder;
builder.append("first"_sd, 1);
builder.append("second"_sd, "str");

std::vector<int> vec = {1, 2, 3};

LOGV2_ERROR(1020, "Example (b: {bson}), (vec: {vector})", 
            "bson"_attr = builder.obj(), 
            "vector"_attr = vec,
            "optional"_attr = boost::none);
```

```
{  
    "t": {  
        "$date": "2020-01-06T19:10:54.246Z"
    },
    "s": "E",
    "c": "NETWORK",
    "ctx": "conn1",
    "id": 23453,
    "msg": "Example (b: {bson}), (vec: {vector})",
    "attr": {  
        "bson": {
            "first": 1,
            "second": "str"
        },
        "vector": [1, 2, 3],
        "optional": null
    }
}
```

## BSON

The BSON formatter is an internal formatter that may or may not be removed. It produces BSON documents close to the JSON document above. Due to the lack of unsigned integer types in BSON, logged C++ types are handled according to the table below for BSON:

C++ Type | BSON Type
-------- | ---------
char | int32 (0x10)
signed char | int32 (0x10)
unsigned char | int32 (0x10)
short | int32 (0x10)
unsigned short | int32 (0x10)
int | int32 (0x10)
unsigned int | int64 (0x12)
long | int64 (0x12)
unsigned long | int64 (0x12)
long long | int64 (0x12)
unsigned long long | int64 (0x12)
