Those tests are kept separate from jstests/concurrency/fsm_workloads/query, as they require the
TPC-H dataset to be fetched from S3, but the test suites that run all the tests in
jstests/concurrency/fsm_workloads/\* do not provide that.
