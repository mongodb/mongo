# TestCases

TestCases extend Python-based `unittest.TestCase` objects that resmoke can run as different "kinds" of tests.

## Supported TestCases

Specify any of the following as the `test_kind` in your [Suite](../../../../buildscripts/resmokeconfig/suites/README.md) config:

- `all_versions_js_test`: [`AllVersionsJSTestCase`](./jstest.py) - Alias for JSTestCase for multiversion passthrough suites.
  - It runs with all combinations of versions of replica sets and sharded clusters. The distinct name is picked up by task generation.
- `benchmark_test`: [`BenchmarkTestCase`](./benchmark_test.py) - A Benchmark test to execute.
- `bulk_write_cluster_js_test`: [`BulkWriteClusterTestCase`](./bulk_write_cluster_js_test.py) - A test to execute with connection data for multiple clusters passed through TestData.
- `cpp_integration_test`: [`CPPIntegrationTestCase`](./cpp_integration_test.py) - A C++ integration test to execute.
- `cpp_libfuzzer_test`: [`CPPLibfuzzerTestCase`](./cpp_libfuzzer_test.py) - A C++ libfuzzer test to execute.
- `cpp_unit_test`: [`CPPUnitTestCase`](./cpp_unittest.py) - A C++ unit test to execute.
- `db_test`: [`DBTestCase`](./dbtest.py) - A dbtest to execute.
- `fsm_workload_test`: [`FSMWorkloadTestCase`](./fsm_workload_test.py) - A wrapper for several copies of a `_SingleFSMWorkloadTestCase` to execute.
- `js_test`: [`JSTestCase`](./jstest.py) - A wrapper for several copies of a `_SingleJSTestCase` to execute
- `json_schema_test`: [`JSONSchemaTestCase`](./json_schema_test.py) - A JSON Schema test to execute.
- `magic_restore_js_test`: [`MagicRestoreTestCase`](./magic_restore_js_test.py) - A test to execute for running tests in a try/catch block.
- `mongos_test`: [`MongosTestCase`](./mongos_test.py) - A TestCase which runs a mongos binary with the given parameters.
- `multi_stmt_txn_passthrough`: [`MultiStmtTxnTestCase`](./multi_stmt_txn_test.py) - Test case for multi statement transactions.
- `parallel_fsm_workload_test`: [`ParallelFSMWorkloadTestCase`](./fsm_workload_test.py) - An FSM workload to execute.
- `pretty_printer_test`: [`PrettyPrinterTestCase`](./pretty_printer_testcase.py) - A pretty printer test to execute.
- `py_test`: [`PyTestCase`](./pytest.py) - A python test to execute.
- `query_tester_self_test`: [`QueryTesterSelfTestCase`](./query_tester_self_test.py) - A QueryTester self-test to execute.
- `query_tester_server_test`: [`QueryTesterServerTestCase`](./query_tester_server_test.py) - A QueryTester server test to execute.
- `sdam_json_test`: [`SDAMJsonTestCase`](./sdam_json_test.py) - Server Discovery and Monitoring JSON test case.
- `server_selection_json_test`: [`ServerSelectionJsonTestCase`](./server_selection_json_test.py) - Server Selection JSON test case.
- `sleep_test`: [`SleepTestCase`](./sleeptest.py) - SleepTestCase class.
- `tla_plus_test`: [`TLAPlusTestCase`](./tla_plus_test.py) - A TLA+ specification to model-check.

## Interfaces

Top level interfaces:

- [`TestCase`](./interface.py) - A test case to execute. The `run_test` method must be implemented.
- [`ProcessTestCase`](./interface.py) - Base class for TestCases that executes an external process. The `_make_process` method must be implemented.

Subclasses:

- [`FixtureTestCase`](./fixture.py) - Base class for the fixture test cases.
- [`FixtureSetupTestCase`](./fixture.py) - TestCase for setting up a fixture.
- [`FixtureTeardownTestCase`](./fixture.py) - TestCase for tearing down a fixture.
- [`FixtureAbortTestCase`](./fixture.py) - TestCase for killing a fixture. Intended for use before archiving a failed test.
- [`JSRunnerFileTestCase`](./jsrunnerfile.py) - A test case with a static JavaScript runner file to execute.
- [`MultiClientsTestCase`](./jstest.py) - A wrapper for several copies of a SingleTestCase to execute.
- [`TestCaseFactory`](./interface.py) - Convenience interface to initialize and build test cases

## Testing TestCases

Self-tests for the testcases themselves can be found in [buildscripts/tests/resmokelib/testing/testcases/](../../../../buildscripts/tests/resmokelib/testing/testcases/)
