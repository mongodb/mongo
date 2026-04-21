# Introduction

The plan stability tests for join optimization are golden tests that execute a number of joins
against the TPC-H dataset.

For each pipeline we persist the following in the golden test output:

- the MQL command, including the base table and the pipeline
- a concise representation of the winning plan for the query
- execution counters that quantify the effort it took to execute the query in terms of docs and keys
  examined
- data about the resultset, such as the number of rows returned

## Prerequisites

This test requires the following:

- The `mongorestore` tool, accessible on the $PATH. This tool is part of the
  [MongoDB Database Tools](https://www.mongodb.com/try/download/database-tools) package.

- The TPC-H dataset, located in a directory named `tpc-h` that is on the same level as the mongodb
  repository. The dataset is available from the `query-benchmark-data` S3 bucket. You can retrieve
  it as follows:

```bash
mkdir ~/tpc-h

aws configure sso
aws sso login

aws s3 cp s3://query-benchmark-data/tpc-h/tpch-0.1-normalized.archive.gz tpc-h/tpch-0.1-normalized.archive.gz --region us-east-1
```

In evergreen, tasks such as `query_golden_join_optimization_plan_stability` make sure the
prerequisites are already in place.

- The golden test framework configured with a custom diff rule

```bash
cd ~/mongo
buildscripts/golden_test.py setup

cat >> ~/.golden_test_config.yml <<'EOF'
diffCmd: 'git -c diff.plan_stability.xfuncname=">>>" diff --unified=0 --function-context --no-index "{{expected}}" "{{actual}}"'
EOF
```

## Running

```bash
buildscripts/resmoke.py run \
--suites=query_golden_join_optimization_plan_stability \
jstests/query_golden/join_opt/plan_stability_* \
--runAllFeatureFlags
```

## Visualizing plan differences

To get a simple diff of all the plans that have changed:

```bash
buildscripts/golden_test.py diff
```

To get a summary report on the failures:

```bash
git clone git@github.com:10gen/feature-extractor.git
cd feature-extractor

bin/venv scripts/join_optimization/summary.py \
--uuid `~/mongo/buildscripts/golden_test.py latest` \
--test-glob 'plan_stability*' \
> report.md
```

You can then open the resulting .md file in your favorite Markdown viewer, such as VSCode.

The report contains the following information:

- a summary of the number of queries that regressed, improved or had ambiguous counters;
- the most-regressed queries, useful as a starting point for debugging;
- the most-improved queries, useful for obtaining examples for presentation purposes;
- all individual failures, categorized and pretty-printed.

The report has one section per jstest -- if you are running multiple tests, each one will be
processed and reported separately.

## Debugging

> [!WARNING] > **_WARNING:_** The queries dumped by this test, the diff tooling or the summary
> report may contain EJSON constructs, such as $numberDecimal that are not properly processed by
> `coll.aggregate()` unless converted using `EJSON.parse()`. Typing information around ISO dates may
> have also been lost, so manually recreate those as `ISODate(...)`. See the "A note on the queries"
> section below for more information.

### Determining the offending query

Each query has an `idx` key that can be used to track it across files and reports.

### Starting a populated MongoDB instance

To obtain a running, populated MongoDB instance, run `resmoke.py run` with the
`--pauseAfterPopulate` option. This will start mongod, load the data and then pause resmoke at the
following line:

```
[js_test:plan_stability_join_opt_tpch] [jsTest] TestData.pauseAfterPopulate is set. Pausing indefinitely ...
```

You can then access the MongoDB instance at the default port for testing:

```bash
mongosh mongodb://127.0.0.1:20000

> use db plan_stability_test_name_goes_here
```

Note that the data will usually be loaded in a database whose name matches the name of the test.

### Loading the data into an existing instance

You can call `mongorestore` directly:

```bash
cd tpc-h
mongorestore \
--uri mongodb://localhost:20000 \
--maintainInsertionOrder \
--gzip \
--archive tpch-0.1-normalized.archive.gz
```

The collections will be restored to the `tpch` database.

## A note on the queries

The queries you see in files, diffs, bug reports may be in various formats, depending on whether
they were dumped using JavaScript, python, or some other method.

Therefore, it is important to obtain the query plan of the query and make sure that what you are
seeing locally matches the plan from the bug report.

The following caveats are currently known:

### Typing information for timestamps

Typing information for timestamps is frequently lost, so a query may contain ISO timestamps as
strings:

```json
{"l_commitdate": {"$lt": "1993-03-17T00:00:00"}}
```

Such a predicate is unlikely to match any rows, so your query will not behave as it originally did.

You will need to manually convert this into a timestamp:

```json
{'l_commitdate': {'$lt': new ISODate('1993-03-17T00:00:00')}}
```

Since the typing information has been lost somewhere along the pipeline, no existing library is
available to restore it for you.

### EJSON output

Sometimes the query will be provided in EJSON, so you will see:

```json
{$regex: {$regex: ... }}
```

in the output.

In mongosh, `aggregate()` does not support EJSON directly, so passing EJSON to it will succeed but
will not produce the expected results.

Either pass this output as `EJSON.parse()` (if your input is a string), `EJSON.deserialize()` (if
your input is parsed already) or manually convert it to standard MQL.
