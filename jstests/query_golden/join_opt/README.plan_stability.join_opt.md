# Introduction

The plan stability tests for join optimization are golden tests that execute a number of joins against the TPC-H dataset.

For each pipeline we persist the following in the golden test output:

- the MQL command, including the base table and the pipeline
- a concise representation of the winning plan for the query
- execution counters that quantify the effort it took to execute the query in terms of docs and keys examined
- data about the resultset, such as the number of rows returned

## Prerequisites

This test requires the following:

- The `mongorestore` tool, accessible on the $PATH. This tool is part of the [MongoDB Database Tools](https://www.mongodb.com/try/download/database-tools) package.

- The TPC-H dataset, located in a directory named `tpc-h` that is on the same level as the mongodb repository. The dataset is available from the `query-benchmark-data` S3 bucket.

In evergreen, tasks such as `query_golden_join_optimization_plan_stability` make sure the prerequisites are already in place.

## Running

```
buildscripts/resmoke.py run --suites=query_golden_join_optimization_plan_stability jstests/query_golden/join_opt/plan_stability_*
```

followed by

```
buildscripts/golden_test.py diff
```

to view any differences as compared to the expected output.

## Debugging

To obtain a running, populated MongoDB instance, start `resmoke.py run` with the `--pauseAfterPopulate` option. This will start mongod, load the data and then pause resmoke at
the following line:

```
[js_test:plan_stability_join_opt_tpch] [jsTest] TestData.pauseAfterPopulate is set. Pausing indefinitely ...
```

You can then access the MongoDB instance at the default port for testing:

```
 mongosh mongodb://127.0.0.1:20000
```

Note that the data will usually be loaded in a database whose name matches the name of the test.

## A note on the queries

The queries you see in files, diffs, bug reports may be in various formats, depending on whether they were dumped using JavaScript, python, or some other method.

Therefore, it is important to obtain the query plan of the query and make sure that what you are seeing locally matches the plan from the bug report.

The following caveats are currently known:

### Typing information for timestamps

Typing information for timestamps is frequently lost, so a query may contain ISO timestamps as strings:

```
{'l_commitdate': {'$lt': '1993-03-17T00:00:00'}}
```

Such a predicate is unlikely to match any rows, so your query will not behave as it originally did.

You will need to manually convert this into a timestamp:

```
{'l_commitdate': {'$lt': new ISODate('1993-03-17T00:00:00')}}
```

Since the typing information has been lost somewhere along the pipeline, no existing library is available to restore it for you.

### EJSON output

Sometimes the query will be provided in EJSON, so you will see:

```
{$regex: {$regex: ... }}
```

in the output.

mongosh's `aggregate()` does not support EJSON directly, so passing EJSON to it is not going to produce the expected results.

Either pass this output as `EJSON.parse()` (if your input is a string), `EJSON.deserialize()` (if your input is parsed already) or manually convert it to standard MQL.
