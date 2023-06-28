# The cppsuite test framework
The cppsuite is a C++ framework designed to help developers write multithreaded and highly configurable testing for WiredTiger. It is intended to make stress testing as well as scenario-driven testing easy to implement. The framework provides built-in [components](#components) that each offers a set of features to enhance the testing experience and [database operations](#database-operations) with a default implementation that can be overridden at the test level. Each test has a corresponding [configuration file](#test-configuration-file) that defines the workload.

## Table of contents
- [Database operations](#database-operations)
- [Components](#components)
- [Test configuration file](#test-configuration-file)
- [Logging](#logging)
- [Tutorial](#tutorial)

## Database operations
The database operations and their default implementation are described in the subsections below. During a test, a user-defined number of threads can be associated to each of those operations. Currently, the framework only supports operations on keys and values of type string. The values are randomly generated using a uniform distribution.

### Populate
Populate is the first operation called in a test, before any other operation. It fills the database with an initial dataset. The number of collections, keys per collection, size of keys and values are defined in the configuration file. For each collection, we insert the configured number of key/value pairs which are all unique. If more than one thread is configured for this operation, the framework divides the number of collections across the number of threads.

### Insert
An insert operation inserts a unique key with a random value into a collection. The number of insert threads, insertions performed in each transaction, sleep time between each insert, size of keys and values are defined in the configuration file. The insert threads are evenly divided between the number of collections created during the populate phase. Only one thread works on a collection at any given time.

### Update
An update operation uses a random cursor to select a key in the database and updates it with a random value. The number of update threads, updates performed in each transaction, sleep time between each update, size of keys and values are defined in the configuration file.

### Remove
A remove operation uses a random cursor to select and remove a key from the database. The number of remove threads, removals performed in each transaction and sleep time between each removal are defined in the configuration file.

### Read
A read operation selects a random collection and uses a random cursor to read items from it. The number of read threads, reads performed in each transaction and the sleep time between each read are defined in the configuration file.

### Custom
There is no default implementation as this function is intended to be user-defined. The number of threads, operations performed in each transaction, sleep time between each operation, size of the keys and values are defined in the configuration file. 

### Checkpoint
A checkpoint operation executes a checkpoint on the database every 60 seconds. Checkpoints are enabled or disabled by assigning one or zero threads in the configuration file, and are enabled by default. The checkpoint frequency is defined in the configuration file.

### Validate
The default validation algorithm requires the default [operation tracker](#operation-tracker) configuration. If the operation tracker is reconfigured, the default validation is skipped. The default validator checks if the WiredTiger tables are consistent with the tables tracked by the operation tracker by comparing their content. The validation (and hence the test) fails if there is any mismatch.

## Components
The framework provides built-in components that each offer a set of features to enhance the testing experience. Their behavior is customized through a configuration file.  A component has a life cycle made of three stages: *load*, *run*, and *finish*. Each of these stages is described for each component below.


### Workload manager
The workload manager is responsible for calling the populate function and the lifecycle of the threads dedicated to each of the database operations described above.

| Phase    | Description |
| -------- | ----------- |
| Load     | N/A         |
| Run      | Calls the populate function, then parses the configuration for each operation and spawns threads for each of them. |
| Finish   | Ends each thread started in the run phase. |

### Operation tracker
During the execution of a test, the operation tracker saves schema and non-schema operations to their own respective tables.

#### Schema operations
The cppsuite schema operations represent the creation and deletion of tables. This data is automatically saved during the `populate` operation via the `save_schema_operation` function. The user cannot customize what data is saved. The table below represents the data associated with each record:

| Key                      | Value            |
| ------------------------ | ---------------- |
| collection id, timestamp | schema operation |

The `collection id` is the identifier of the collection the schema operation is applied to. The `timestamp` is the time at which the operation occurred. The `schema operation` is a value from the enumeration `tracking_operation`, either `CREATE_COLLECTION` or `DELETE_COLLECTION`.

#### Non-schema operations
Non-schema operations in the cppsuite are insertions, removals or updates of records which are automatically saved during the `insert`, `update`, and `remove` operations via the `save_operation` function. The framework defines a set of default data to track in this table during a test but the user can customize it by editing the test configuration file and overriding the `set_tracking_cursor` function. The table below represents the data associated with each record by default:

| Key                           | Value            |
| ----------------------------- | ---------------- |
| collection id, key, timestamp | operation, value |

The `collection id` is the identifier of the collection the schema operation is applied to. The `timestamp` is the time at which the operation occurred. The `operation` is a value from the enumeration `tracking_operation`, either `CUSTOM`, `DELETE_KEY` or `INSERT`. The `key` and `value` represent the key/value pair the operation is applied to. For the database operation `remove`, the field `value` is empty.

Any saved data can be used at the [validation](#validate) stage.

| Phase    | Description |
| -------- | ----------- |
| Load     | Creates the tables required to track the test data. |
| Run      | Prunes any data not applicable to the validation stage when the framework tracks the default data. |
| Finish   | N/A |

### Timestamp manager
The timestamp manager is responsible for managing the stable and oldest timestamps as well as providing timestamps to the user on demand. The stable and oldest timestamps are updated according to the definitions of the `oldest_lag` and `stable_lag` in the test configuration file. The `oldest_lag` is the difference in time between the oldest and stable timestamps, and `stable_lag` is the difference in time between the stable timestamp and the current time. When the timestamp manager updates the different timestamps it is possible that concurrent operations may fail. For instance, if the oldest timestamp moves to a time more recent than a commit timestamp of a concurrent transaction, the transaction will fail. 

| Phase    | Description |
| -------- | ----------- |
| Load     | Initializes the oldest and stable lags using the values retrieved from the configuration file. |
| Run      | Updates the oldest and stable lags as well as the oldest and stable timestamps. |
| Finish   | N/A |

### Metrics monitor
The metrics monitor polls database metrics during and after test execution. If a metric is found to be outside the acceptable range defined in the configuration file then the test will fail. When a test ends, the metrics monitor can export the value of these statistics to a JSON file that can be uploaded to Evergreen or Atlas.

| Phase    | Description |
| -------- | ----------- |
| Load     | Retrieves the metrics from the configuration file that need to be monitored during and after the test. |
| Run      | Checks runtime metrics and fails the test if one is found outside the range defined in the configuration file. |
| Finish   | Checks post run metrics and fails the test if one is found outside the range defined in the configuration file. Exports any required metrics to a JSON file if the test is successful. |

The components and their implementation can be found in the [component](https://github.com/wiredtiger/wiredtiger/tree/develop/test/cppsuite/src/component) folder.

## Test configuration file
A test configuration is a text file that is associated with a test and defines the workload. The test configuration file contains top-level settings such as the test duration or cache size and component level settings that define their behavior. The format is the following:

```cpp
# Top level settings
duration_seconds=15,
cache_size_mb=200,
...,
# Components settings
timestamp_manager=
(
  enabled=true,
  oldest_lag=1,
  stable_lag=1,
  ...,
),
...,
```

A configuration file is required to run a test. Each test can have one or more associated configuration files. The configurable fields and default settings are defined in [test_data.py](https://github.com/wiredtiger/wiredtiger/blob/develop/dist/test_data.py).

## Logging
The framework writes traces to stdout using a [logger](https://github.com/wiredtiger/wiredtiger/blob/develop/test/cppsuite/src/common/logger.cpp) and supports the following log levels:

| Logging level | Logging value | Description |
| ------------- | ------------- | ----------- |
| 0             | LOG_ERROR     | Message indicated an error faced by the framework or a failure of the test. |
| 1             | LOG_WARN      | Warning conditions potentially signaling non-imminent errors and behaviors. |
| 2             | LOG_INFO      | Informational style messages. |
| 3             | LOG_TRACE     | Low severity messages, useful for debugging purposes. |

A message will not be filtered if the configured logging level is higher or equal to the logging level of the message:

- The `LOG_WARN` level prints `LOG_WARN` and `LOG_ERROR` messages.
- The `LOG_INFO` level prints `LOG_INFO`, `LOG_WARN` and `LOG_ERROR` messages.
- The `LOG_TRACE` level prints `LOG_TRACE`, `LOG_INFO`, `LOG_WARN` and `LOG_ERROR` messages.

To call the logger both a logging level and message need to be provided:

```cpp
logger::log_msg(LOG_TRACE, "A logging message with the LOG_TRACE level");

logger::log_msg(LOG_INFO, "A logging message with the LOG_INFO level");

logger::log_msg(LOG_WARN, "A logging message with the LOG_WARN level");

logger::log_msg(LOG_ERROR, "A logging message with the LOG_ERROR level");
```

The log level is specified as a command line argument when running a test.

## Tutorial
Learn how to create and run cppsuite tests in [How to use cppsuite](how_to_use_cppsuite.md).