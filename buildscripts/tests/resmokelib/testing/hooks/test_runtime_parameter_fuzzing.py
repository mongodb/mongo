"""Unit tests for buildscripts/resmokelib/testing/hooks/fuzz_runtime_parameters.py."""

import random
import sys
import unittest

import mock

from buildscripts.resmokelib.generate_fuzz_config.mongo_fuzzer_configs import (
    generate_special_runtime_parameters,
)
from buildscripts.resmokelib.testing.hooks import fuzz_runtime_parameters as _runtime_fuzzer


class TestRuntimeFuzzGeneration(unittest.TestCase):
    def assert_parameter_values_ok(self, spec, generated_values):
        for name, val in generated_values.items():
            options = spec[name]
            if "isRandomizedChoice" in options:
                lb = options["lower_bound"]
                ub = options["upper_bound"]
                self.assertTrue(lb <= val <= ub)
            elif "choices" in options:
                self.assertIn(val, options["choices"])
            elif "min" and "max" in options:
                self.assertTrue(options["min"] <= val <= options["max"])
            else:
                self.assertIn("default", options)
                self.assertEqual(val, options["default"])

    @mock.patch("buildscripts.resmokelib.testing.hooks.fuzz_runtime_parameters.time.time")
    def test_frequency_respected(self, mock_time):
        start_time = 1625140800
        mock_time.return_value = start_time

        test_runtime_params = {
            "mongod": {
                "ShardingTaskExecutorPoolMinSize": {"min": 1, "max": 50, "period": 5},
                "ingressAdmissionControllerTicketPoolSize": {
                    "choices": [500_000, 1_000_000, 2_000_000],
                    "lower_bound": 1000,
                    "upper_bound": 5_000_000,
                    "isRandomizedChoice": True,
                    "period": 1,
                },
                "ingressAdmissionControlEnabled": {
                    "choices": [True, False],
                    "period": 10,
                },
            },
            "mongos": {
                "ShardingTaskExecutorPoolMinSize": {"min": 1, "max": 50, "period": 5},
            },
        }

        mongod_spec = test_runtime_params["mongod"]
        runtimeFuzzerParamState = _runtime_fuzzer.RuntimeParametersState(
            mongod_spec, random.randrange(sys.maxsize)
        )
        # No time has passed; we wouldn't want to set any of these yet.
        ret = runtimeFuzzerParamState.generate_parameters()
        self.assertEqual(ret, {})

        mock_time.return_value = start_time + 1
        ret = runtimeFuzzerParamState.generate_parameters()

        # We should set ingressAdmissionControllerTicketPoolSize now, but not ingressAdmissionControlEnabled or ShardingTaskExecutorPoolMinSize
        param_names_to_set = ret.keys()
        self.assertIn("ingressAdmissionControllerTicketPoolSize", param_names_to_set)
        self.assertNotIn("ingressAdmissionControlEnabled", param_names_to_set)
        self.assertNotIn("ShardingTaskExecutorPoolMinSize", param_names_to_set)
        self.assert_parameter_values_ok(mongod_spec, ret)

        # Don't advance time, and generate the values again. Since no time has passed, nothing should be set.
        ret = runtimeFuzzerParamState.generate_parameters()
        self.assertEqual(ret, {})

        # Now advance the time enough such that ShardingTaskExecutorPoolMinSize should be set also.
        mock_time.return_value = start_time + 5
        ret = runtimeFuzzerParamState.generate_parameters()
        param_names_to_set = ret.keys()
        self.assertIn("ingressAdmissionControllerTicketPoolSize", param_names_to_set)
        self.assertIn("ShardingTaskExecutorPoolMinSize", param_names_to_set)
        self.assertNotIn("ingressAdmissionControlEnabled", param_names_to_set)
        self.assert_parameter_values_ok(mongod_spec, ret)

        # Don't advance time, and generate the values again. Since no time has passed, nothing should be set.
        ret = runtimeFuzzerParamState.generate_parameters()
        self.assertEqual(ret, {})

        # Now advance the time enough such that all 3 should be set.
        mock_time.return_value = start_time + 10
        ret = runtimeFuzzerParamState.generate_parameters()
        param_names_to_set = ret.keys()
        self.assertIn("ingressAdmissionControllerTicketPoolSize", param_names_to_set)
        self.assertIn("ShardingTaskExecutorPoolMinSize", param_names_to_set)
        self.assertIn("ingressAdmissionControlEnabled", param_names_to_set)
        self.assert_parameter_values_ok(mongod_spec, ret)

    def test_special_runtime_param_known_returns_in_range(self):
        rng = random.Random(42)
        for _ in range(50):
            val = generate_special_runtime_parameters(rng, "flowControlThresholdLagPercentage", {})
            self.assertIsInstance(val, float)
            self.assertGreaterEqual(val, 0.0)
            self.assertLess(val, 1.0)  # rng.random() is [0.0, 1.0)

    def test_special_runtime_param_unknown_raises(self):
        with self.assertRaises(ValueError):
            generate_special_runtime_parameters(random.Random(0), "unknownSpecialParam", {})

    @mock.patch("buildscripts.resmokelib.testing.hooks.fuzz_runtime_parameters.time.time")
    def test_custom_fuzz_value_assignment_dispatches_to_special_handler(self, mock_time):
        start_time = 1625140800
        mock_time.return_value = start_time

        spec = {
            "flowControlThresholdLagPercentage": {
                "min": 0.0,
                "max": 1.0,
                "custom_fuzz_value_assignment": True,
                "period": 1,
            },
        }
        state = _runtime_fuzzer.RuntimeParametersState(spec, seed=42)

        mock_time.return_value = start_time + 1
        ret = state.generate_parameters()
        self.assertIn("flowControlThresholdLagPercentage", ret)
        val = ret["flowControlThresholdLagPercentage"]
        self.assertIsInstance(val, float)
        self.assertGreaterEqual(val, 0.0)
        self.assertLess(val, 1.0)

    def test_runtime_param_spec_validation(self):
        bad_spec_value_not_dict = {"fakeRuntimeParam": 1}
        bad_spec_value_no_period = {"fakeRuntimeParam": {"max": 50, "min": 10}}
        good_spec = {"fakeRuntimeParam": {"max": 50, "min": 10, "period": 5}}

        with self.assertRaises(ValueError):
            _runtime_fuzzer.validate_runtime_parameter_spec(bad_spec_value_not_dict)

        with self.assertRaises(ValueError):
            _runtime_fuzzer.validate_runtime_parameter_spec(bad_spec_value_no_period)
        # No exception for good dict
        _runtime_fuzzer.validate_runtime_parameter_spec(good_spec)
