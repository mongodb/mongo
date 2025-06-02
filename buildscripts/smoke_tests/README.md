# Smoke Test Suites

Smoke test suites are suites that are meant to be run locally during iterative development as a way
to get a decent sense of whether a change to a given component has not broken the core functionality
of that component. They are not meant to replace running a patch build before merging in a change,
but are a way to get relatively quick feedback locally.

# Available Smoke Test Suites

Below is information about the smoke test suites available for each team and how to run them. More
information can be found in the respective .yml files for each team/component within this directory.

## Catalog And Routing

The integration tests for the smoke test suite can be run with the following command:

```
python buildscripts/run_smoke_tests.py --suites=catalog_and_routing
```

## Server Integration

The integration tests for the smoke test suite can be run with the following command:

```
bazel build install-devcore && python buildscripts/run_smoke_tests.py --suites=server_integration
```

This should be run in conjunction with the unit tests, which can be run with the following:

```
bazel test --test_tag_filters=server-integration-smoke //...
```

## Replication

The integration tests for the smoke test suite can be run with the following command:

```
bazel build install-dist-test && python buildscripts/run_smoke_tests.py --suites=replication
```

This should be run in conjunction with the unit tests, which can be run with the following:

```
bazel test --test_output=summary //src/mongo/db/repl/...
```

## Server Programmability

Running the unit tests and integration tests together can be accomplished with the following:

```
bazel test --test_tag_filters=server-programmability //...
bazel build install-devcore && python buildscripts/run_smoke_tests.py --suites=server_programmability
```

## Storage Execution

The smoke test suites for storage execution are divided up into components. The smoke test suite
for all of the components that storage execution owns can be run with the following:

```
bazel test --test_tag_filters=server-bsoncolumn,server-collection-write-path,server-external-sorter,server-index-builds,server-key-string,server-storage-engine-integration,server-timeseries-bucket-catalog,server-tracking-allocators,server-ttl //src/mongo/...
bazel build install-dist-test && python buildscripts/run_smoke_tests.py --suites=storage_execution
```

The individual components owned by storage execution are as follows:

### Server-BSONColumn

The unit tests for the server-bsoncolumn component can be run with the following:

```
bazel test --test_tag_filters=server-bsoncolumn --test_output=summary //src/mongo/...
```

There are currently no smoke test integration tests for this component.

### Server-Collection-Write-Path

The unit and integration tests for the server-collection-write-path component can be run with the following:

```
bazel test --test_tag_filters=server-collection-write-path --test_output=summary //src/mongo/...
bazel build install-dist-test && python buildscripts/run_smoke_tests.py --suites=server_collection_write_path
```

### Server-External-Sorter

The unit tests for the server-external-sorter component can be run with the following:

```
bazel test --test_tag_filters=server-external-sorter --test_output=summary //src/mongo/...
```

There are currently no smoke test integration tests for this component.

### Server-Index-Builds

The unit and integration tests for the server-index-builds component can be run with the following:

```
bazel test --test_tag_filters=server-index-builds --test_output=summary //src/mongo/...
bazel build install-dist-test && python buildscripts/run_smoke_tests.py --suites=server_index_builds
```

### Server-Key-String

The unit tests for the server-key-string component can be run with the following:

```
bazel test --test_tag_filters=server-key-string --test_output=summary //src/mongo/...
```

There are currently no smoke test integration tests for this component.

### Server-Storage-Engine-Integration

The unit and integration tests for the server-storage-engine-integration component can be run with the following:

```
bazel test --test_tag_filters=server-storage-engine-integration --test_output=summary //src/mongo/...
bazel build install-dist-test && python buildscripts/run_smoke_tests.py --suites=server_storage_engine_integration
```

### Server-Timeseries-Bucket-Catalog

The unit tests for the server-timeseries-bucket-catalog component can be run with the following:

```
bazel test --test_tag_filters=server-timeseries-bucket-catalog --test_output=summary //src/mongo/...
```

There are currently no smoke test integration tests for this component.

### Server-Tracking-Allocators

The unit tests for the server-tracking-allocators component can be run with the following:

```
bazel test --test_tag_filters=server-tracking-allocators --test_output=summary //src/mongo/...
```

There are currently no smoke test integration tests for this component.

### Server-TTL

The unit and integration tests for the server-ttl component can be run with the following:

```
bazel test --test_tag_filters=server-ttl --test_output=summary //src/mongo/...
bazel build install-dist-test && python buildscripts/run_smoke_tests.py --suites=server_ttl
```
