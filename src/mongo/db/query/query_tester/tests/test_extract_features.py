# This test is meant to be run locally to test the --extractFeatures flag works correctly. It is
# excluded from the selfTests folder so that it doesn't run on evergreen. To run the test, start up
# a mongod (default port = 27017) and run the following from the mongo repo root:
#
# python3 src/mongo/db/query/query_tester/tests/test_extract_features.py -b build/install/bin/mongotest -u mongodb://127.0.0.1:27017/

from selfTests.testlib.test_utils import (
    ExitCode,
    Mode,
    assert_exit_code,
    assert_output_contains,
    run_mongotest,
)


def run_test_case(test_name, expected_output_checks):
    """
    Runs a specific test case using `run_mongotest` and verifies the expected features are extracted.

    Args:
        test_name (str): The name of the test file to run (without .test extension).
        expected_output_checks (list): A list of expected feature strings to verify in the output.

    Raises:
        AssertionError: If the exit code is unexpected or the output is missing expected features.
    """
    # Run the mongotest command with --extractFeatures
    exit_code, output = run_mongotest(
        (test_name,),
        Mode.COMPARE,
        drop=True,
        load=True,
        minimal_index=False,
        out_result=False,
        extract_features=True,
    )

    assert_exit_code(exit_code, ExitCode.FAILURE, output)

    for expected in expected_output_checks:
        assert_output_contains(output, expected)


# Test cases to run
test_cases = [
    {
        "test_name": "diff_queries_same_res",
        "expected_output_checks": [
            "diff --git",
            "TestNum: 0",
            'Query: :sortResults {aggregate: "fuzzer_coll", pipeline: [{$limit: 1}], cursor: {}}',
            "TestNum: 1",
            'Query: :sortResults {aggregate: "fuzzer_coll", pipeline: [{$limit: 2}], cursor: {}}',
            "Query Features:",
            "PipelineFirstStage:",
            "PipelineStage:",
            "- $limit",
            "PlanStage:",
            "PlanStageQuality:",
            "- COLLSCAN",
            "- LIMIT",
            "PlanStageRelationship:",
            "- LIMIT->COLLSCAN",
            "PlannerProperty:",
            "- optimizedPipeline",
            "RejectedPlanCount:",
            "- 0",
            "RejectedPlans:",
            "- ABSENT",
        ],
    },
    {
        "test_name": "same_res_len_diff_res",
        "expected_output_checks": [
            "diff --git",
            "TestNum: 1",
            "Query:",
            ':sortResults {aggregate: "fuzzer_coll", pipeline: [{$sort: {value: -1}}, {$addFields: {_id: {$multiply: [-1, "$_id"]}}}], cursor: {}}',
            "Query Features:",
            "IndexProperty:",
            "- Calculated_keyPattern_single",
            "- Calculated_partialKeyRange",
            "Operator:",
            "- $multiply",
            "PipelineFirstStage:",
            "- $sort",
            "PipelineStage",
            "- $addFields",
            "PipelineStageRelationship:",
            "- $sort->$addFields",
            "PlanStage:",
            "PlanStageQuality:",
            "- FETCH",
            "- IXSCAN",
            "PlanStageRelationship:",
            "- FETCH->IXSCAN",
            "RejectedPlanCount",
            "- 0",
            "RejectedPlans",
            "- ABSENT",
        ],
    },
]

# Run each test case
for case in test_cases:
    run_test_case(case["test_name"], case["expected_output_checks"])
