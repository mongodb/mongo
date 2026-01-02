# Smoke Test Suites

Smoke test suites are suites that are meant to be run locally during iterative development as a way
to get a decent sense of whether a change to a given component has not broken the core functionality
of that component. They are not meant to replace running a patch build before merging in a change,
but are a way to get relatively quick feedback locally.

# Available Smoke Test Suites

Below is information about the smoke test suites available for each team and how to run them.

## Catalog And Routing

Tests are tagged with `catalog-and-routing`. To run:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,catalog-and-routing //...
```

## Server Integration

Tests are tagged with `server-integration-smoke`. To run:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,server-integration-smoke //...
```

## Replication

The integration tests for the smoke test are tagged with `replication-smoke`. To run:

```
bazel test --test_tag_filters=replication-smoke //...
```

This should be run in conjunction with the unit tests, which can be run with the following:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug //src/mongo/db/repl/...
```

To run both combined:

```
bazel test --test_output=summary --test_tag_filters=mongo_unittest,replication-smoke,-wrapper_target,-intermediate_debug //src/mongo/db/repl/... //buildscripts/smoke_tests/...
```

## Server Programmability

Tests are tagged with `server-programmability`. To run:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,server-programmability //...
```

## Storage Execution

The smoke test suites for storage execution are divided up into components. The smoke test suite
for all of the components that storage execution owns can be run with the following:

```
bazel test --test_output=summary  --test_tag_filters=-wrapper_target,-intermediate_debug,server-bsoncolumn,server-collection-write-path,server-external-sorter,server-index-builds,server-key-string,server-storage-engine-integration,server-timeseries-bucket-catalog,server-tracking-allocators,server-ttl //...
```

The individual components owned by storage execution are as follows:

### Server-BSONColumna

The unit tests for the server-bsoncolumn component can be run with the following:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,server-bsoncolumn //...
```

There are currently no smoke test integration tests for this component.

### Server-Collection-Write-Path

The unit and integration tests for the server-collection-write-path component can be run with the following:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,server-collection-write-path //...
```

### Server-External-Sorter

The unit tests for the server-external-sorter component can be run with the following:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,server-external-sorter //...
```

There are currently no smoke test integration tests for this component.

### Server-Index-Builds

The unit and integration tests for the server-index-builds component can be run with the following:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,server-index-builds //...
```

### Server-Key-String

The unit tests for the server-key-string component can be run with the following:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,server-key-string //...
```

There are currently no smoke test integration tests for this component.

### Server-Storage-Engine-Integration

The unit and integration tests for the server-storage-engine-integration component can be run with the following:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,server-storage-engine-integration //...
```

### Server-Timeseries-Bucket-Catalog

The unit tests for the server-timeseries-bucket-catalog component can be run with the following:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,server-timeseries-bucket-catalog //...
```

There are currently no smoke test integration tests for this component.

### Server-Tracking-Allocators

The unit tests for the server-tracking-allocators component can be run with the following:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,server-tracking-allocators //...
```

There are currently no smoke test integration tests for this component.

### Server-TTL

The unit and integration tests for the server-ttl component can be run with the following:

```
bazel test --test_output=summary --test_tag_filters=-wrapper_target,-intermediate_debug,server-ttl //...
```
