"""Unit tests for buildscripts/resmokelib/generate_fuzz_config/mongo_fuzzer_configs.py."""

import random
import unittest
from unittest import mock

import yaml

from buildscripts.resmokelib import config
from buildscripts.resmokelib.generate_fuzz_config.config_fuzzer_wt_limits import min_trigger_bytes
from buildscripts.resmokelib.generate_fuzz_config.mongo_fuzzer_configs import (
    flow_control_params,
    fuzz_mongod_set_parameters,
    fuzz_mongos_set_parameters,
    generate_eviction_configs,
    generate_mongod_parameters,
    generate_normal_mongo_parameters,
    generate_normal_wt_parameters,
    generate_special_eviction_configs,
    generate_special_table_configs,
    generate_table_configs,
    is_enterprise_param_available,
    safe_randint,
)


def _rng(seed=42):
    return random.Random(seed)


class TestSafeRandint(unittest.TestCase):
    def test_returns_value_in_range(self):
        rng = _rng()
        for _ in range(50):
            val = safe_randint(rng, 1, 10)
            self.assertGreaterEqual(val, 1)
            self.assertLessEqual(val, 10)

    def test_float_min_raises_type_error(self):
        with self.assertRaises(TypeError):
            safe_randint(_rng(), 1.0, 10)

    def test_float_max_raises_type_error(self):
        with self.assertRaises(TypeError):
            safe_randint(_rng(), 1, 10.5)

    def test_error_message_includes_param_name(self):
        with self.assertRaises(TypeError) as ctx:
            safe_randint(_rng(), 1.0, 10, "myParam")
        self.assertIn("myParam", str(ctx.exception))

    def test_error_message_without_param_name_still_raises(self):
        with self.assertRaises(TypeError):
            safe_randint(_rng(), 1, 10.0)


class TestGenerateNormalWtParameters(unittest.TestCase):
    def test_choices_branch(self):
        rng = _rng()
        spec = {"choices": ["none", "snappy", "zlib"]}
        for _ in range(20):
            self.assertIn(generate_normal_wt_parameters(rng, spec), spec["choices"])

    def test_choices_with_multiplier(self):
        rng = _rng()
        spec = {"choices": [4, 8, 12], "multiplier": 1024}
        expected = {4 * 1024, 8 * 1024, 12 * 1024}
        for _ in range(20):
            self.assertIn(generate_normal_wt_parameters(rng, spec), expected)

    def test_min_max_branch(self):
        rng = _rng()
        spec = {"min": 10, "max": 100}
        for _ in range(50):
            val = generate_normal_wt_parameters(rng, spec)
            self.assertGreaterEqual(val, 10)
            self.assertLessEqual(val, 100)


class TestGenerateNormalMongoParameters(unittest.TestCase):
    def test_choices_branch(self):
        rng = _rng()
        spec = {"choices": [True, False]}
        for _ in range(20):
            self.assertIn(generate_normal_mongo_parameters(rng, spec), [True, False])

    def test_min_max_branch(self):
        rng = _rng()
        spec = {"min": 1, "max": 100}
        for _ in range(50):
            val = generate_normal_mongo_parameters(rng, spec)
            self.assertGreaterEqual(val, 1)
            self.assertLessEqual(val, 100)

    def test_min_max_multiplier_branch(self):
        rng = _rng()
        spec = {"min": 1, "max": 10, "multiplier": 1024}
        for _ in range(20):
            val = generate_normal_mongo_parameters(rng, spec)
            self.assertEqual(val % 1024, 0)
            self.assertGreaterEqual(val, 1024)
            self.assertLessEqual(val, 10 * 1024)

    def test_is_uniform_branch(self):
        rng = _rng()
        spec = {"isUniform": True, "min": 0.1, "max": 1.0}
        for _ in range(50):
            val = generate_normal_mongo_parameters(rng, spec)
            self.assertIsInstance(val, float)
            self.assertGreaterEqual(val, 0.1)
            self.assertLessEqual(val, 1.0)

    def test_is_randomized_choice_branch(self):
        # Each call mutates spec["choices"] in place (existing behavior); the result
        # is always within [lower_bound, upper_bound] because both original choices
        # and the appended randint are in that range.
        rng = _rng()
        for _ in range(20):
            spec = {
                "isRandomizedChoice": True,
                "choices": [100, 1000],
                "lower_bound": 50,
                "upper_bound": 5000,
            }
            val = generate_normal_mongo_parameters(rng, spec)
            self.assertGreaterEqual(val, 50)
            self.assertLessEqual(val, 5000)

    def test_default_branch(self):
        val = generate_normal_mongo_parameters(_rng(), {"default": 12345})
        self.assertEqual(val, 12345)

    def test_document_branch_produces_dict_with_expected_keys(self):
        rng = _rng()
        spec = {
            "document": {
                "samplingRate": {"choices": [0.0, 0.5, 1.0]},
                "maxSize": {"min": 1, "max": 100},
            }
        }
        for _ in range(10):
            val = generate_normal_mongo_parameters(rng, spec)
            self.assertIsInstance(val, dict)
            self.assertIn("samplingRate", val)
            self.assertIn(val["samplingRate"], [0.0, 0.5, 1.0])
            self.assertGreaterEqual(val["maxSize"], 1)
            self.assertLessEqual(val["maxSize"], 100)

    def test_document_exclude_prob_one_excludes_key(self):
        rng = _rng()
        spec = {"document": {"key1": {"exclude_prob": 1.0, "choices": [True, False]}}}
        for _ in range(10):
            val = generate_normal_mongo_parameters(rng, spec)
            self.assertNotIn("key1", val)

    def test_document_exclude_prob_zero_includes_key(self):
        rng = _rng()
        spec = {"document": {"key1": {"exclude_prob": 0.0, "choices": [True, False]}}}
        for _ in range(10):
            val = generate_normal_mongo_parameters(rng, spec)
            self.assertIn("key1", val)


class TestEvictionConfigInvariants(unittest.TestCase):
    """Verify the ordering constraints between eviction parameters across many seeds."""

    def _eviction_result(self, seed):
        from buildscripts.resmokelib.generate_fuzz_config.config_fuzzer_wt_limits import (
            config_fuzzer_params,
        )

        return generate_special_eviction_configs(_rng(seed), {}, config_fuzzer_params["wt"])

    def test_eviction_trigger_gt_eviction_target(self):
        for seed in range(100):
            with self.subTest(seed=seed):
                r = self._eviction_result(seed)
                self.assertGreater(r["eviction_trigger"], r["eviction_target"])

    def test_dirty_trigger_gt_dirty_target(self):
        for seed in range(100):
            with self.subTest(seed=seed):
                r = self._eviction_result(seed)
                self.assertGreater(r["eviction_dirty_trigger"], r["eviction_dirty_target"])

    def test_dirty_trigger_lte_trigger_max(self):
        for seed in range(100):
            with self.subTest(seed=seed):
                r = self._eviction_result(seed)
                self.assertLessEqual(r["eviction_dirty_trigger"], r["trigger_max"])

    def test_updates_target_lt_dirty_target(self):
        for seed in range(100):
            with self.subTest(seed=seed):
                r = self._eviction_result(seed)
                self.assertLess(r["eviction_updates_target"], r["eviction_dirty_target"])

    def test_updates_trigger_lt_dirty_trigger(self):
        for seed in range(100):
            with self.subTest(seed=seed):
                r = self._eviction_result(seed)
                self.assertLess(r["eviction_updates_trigger"], r["eviction_dirty_trigger"])

    def test_dirty_and_updates_triggers_gte_min_trigger_bytes(self):
        for seed in range(100):
            with self.subTest(seed=seed):
                r = self._eviction_result(seed)
                self.assertGreaterEqual(r["eviction_dirty_trigger"], min_trigger_bytes)
                self.assertGreaterEqual(r["eviction_updates_trigger"], min_trigger_bytes)

    def test_eviction_config_string_contains_expected_keys(self):
        config_str = generate_eviction_configs(_rng())
        for key in [
            "debug_mode",
            "eviction_checkpoint_target",
            "eviction_dirty_target",
            "eviction_dirty_trigger",
            "eviction_target",
            "eviction_trigger",
            "eviction_updates_target",
            "eviction_updates_trigger",
            "file_manager",
        ]:
            with self.subTest(key=key):
                self.assertIn(key, config_str)


class TestTableConfigInvariants(unittest.TestCase):
    def _table_result(self, seed):
        from buildscripts.resmokelib.generate_fuzz_config.config_fuzzer_wt_limits import (
            config_fuzzer_params,
        )

        params = config_fuzzer_params["wt_table"]
        excluded = {"memory_page_max_lower_bound", "memory_page_max_upper_bound", "memory_page_max"}
        ret = {
            key: generate_normal_wt_parameters(_rng(seed), value, key)
            for key, value in params.items()
            if key not in excluded
        }
        return generate_special_table_configs(_rng(seed), ret, params)

    def test_memory_page_max_gte_leaf_page_max(self):
        for seed in range(50):
            with self.subTest(seed=seed):
                r = self._table_result(seed)
                self.assertGreaterEqual(r["memory_page_max"], r["leaf_page_max"])

    def test_table_config_string_contains_expected_keys(self):
        config_str = generate_table_configs(_rng())
        for key in [
            "block_compressor",
            "internal_page_max",
            "leaf_page_max",
            "leaf_value_max",
            "memory_page_max",
            "prefix_compression",
            "split_pct",
        ]:
            with self.subTest(key=key):
                self.assertIn(key, config_str)


class TestGenerateMongodParameters(unittest.TestCase):
    """Tests for generate_mongod_parameters."""

    def test_all_flow_control_params_consistent_with_enable_flow_control(self):
        """All flow_control_params must be present iff enableFlowControl=True."""
        for seed in range(100):
            with self.subTest(seed=seed):
                params = generate_mongod_parameters(_rng(seed))
                self.assertIn("enableFlowControl", params)
                self.assertIsInstance(params["enableFlowControl"], bool)
                for key in flow_control_params:
                    if params["enableFlowControl"]:
                        self.assertIn(key, params)
                    else:
                        self.assertNotIn(key, params)

    def test_throughput_probing_concurrency_invariants(self):
        for seed in range(50):
            with self.subTest(seed=seed):
                params = generate_mongod_parameters(_rng(seed))
                initial = params["throughputProbingInitialConcurrency"]
                min_c = params["throughputProbingMinConcurrency"]
                max_c = params["throughputProbingMaxConcurrency"]
                self.assertLessEqual(
                    min_c,
                    initial // 2,
                    f"min={min_c} must be <= initialConcurrency//2={initial // 2}",
                )
                self.assertGreaterEqual(
                    max_c,
                    initial // 2,
                    f"max={max_c} must be >= initialConcurrency//2={initial // 2}",
                )

    def test_session_refresh_absent_when_disabled(self):
        for seed in range(200):
            params = generate_mongod_parameters(_rng(seed))
            if params["disableLogicalSessionCacheRefresh"]:
                self.assertNotIn(
                    "logicalSessionRefreshMillis",
                    params,
                    f"seed={seed}: logicalSessionRefreshMillis must be absent when "
                    "disableLogicalSessionCacheRefresh=True",
                )
                return
        self.fail("No seed in [0, 200) produced disableLogicalSessionCacheRefresh=True")

    def test_session_refresh_present_when_enabled(self):
        for seed in range(200):
            params = generate_mongod_parameters(_rng(seed))
            if not params["disableLogicalSessionCacheRefresh"]:
                self.assertIn(
                    "logicalSessionRefreshMillis",
                    params,
                    f"seed={seed}: logicalSessionRefreshMillis must be present when "
                    "disableLogicalSessionCacheRefresh=False",
                )
                return
        self.fail("No seed in [0, 200) produced disableLogicalSessionCacheRefresh=False")


class TestDeterminism(unittest.TestCase):
    """The same seed must always produce identical output."""

    def test_mongod_same_seed_same_result(self):
        a = fuzz_mongod_set_parameters(seed=42, user_provided_params="{}")
        b = fuzz_mongod_set_parameters(seed=42, user_provided_params="{}")
        self.assertEqual(a, b)

    def test_mongod_different_seeds_different_result(self):
        a = fuzz_mongod_set_parameters(seed=42, user_provided_params="{}")
        b = fuzz_mongod_set_parameters(seed=9999, user_provided_params="{}")
        self.assertNotEqual(a, b)

    def test_mongos_same_seed_same_result(self):
        a = fuzz_mongos_set_parameters(seed=42, user_provided_params="{}")
        b = fuzz_mongos_set_parameters(seed=42, user_provided_params="{}")
        self.assertEqual(a, b)

    def test_mongos_different_seeds_different_result(self):
        a = fuzz_mongos_set_parameters(seed=42, user_provided_params="{}")
        b = fuzz_mongos_set_parameters(seed=9999, user_provided_params="{}")
        self.assertNotEqual(a, b)


class TestUserProvidedParamsOverride(unittest.TestCase):
    """User-supplied params must overwrite any generated values for the same key."""

    def test_user_params_override_mongod_generated(self):
        yaml_out, *_ = fuzz_mongod_set_parameters(seed=42, user_provided_params="syncdelay: 9999")
        self.assertEqual(yaml.safe_load(yaml_out)["syncdelay"], 9999)

    def test_user_params_override_mongos_generated(self):
        yaml_out = fuzz_mongos_set_parameters(
            seed=42, user_provided_params="internalQueryFindCommandBatchSize: 7777"
        )
        self.assertEqual(yaml.safe_load(yaml_out)["internalQueryFindCommandBatchSize"], 7777)


class TestIsEnterpriseParamAvailable(unittest.TestCase):
    """Tests for is_enterprise_param_available. Backfill for fix in SERVER-122534."""

    def test_modules_none_non_enterprise_param_does_not_raise(self):
        """MODULES=None must not raise TypeError for non-enterprise params (regression)."""
        with mock.patch.object(config, "MODULES", None):
            self.assertTrue(is_enterprise_param_available({}))

    def test_modules_none_enterprise_param_does_not_raise(self):
        """MODULES=None must not raise TypeError for enterprise-only params (regression)."""
        with mock.patch.object(config, "MODULES", None):
            self.assertFalse(is_enterprise_param_available({"enterprise_only": True}))

    def test_modules_empty_non_enterprise_param(self):
        with mock.patch.object(config, "MODULES", []):
            self.assertTrue(is_enterprise_param_available({}))

    def test_modules_empty_enterprise_param(self):
        with mock.patch.object(config, "MODULES", []):
            self.assertFalse(is_enterprise_param_available({"enterprise_only": True}))

    def test_modules_with_enterprise_non_enterprise_param(self):
        with mock.patch.object(config, "MODULES", ["enterprise"]):
            self.assertTrue(is_enterprise_param_available({}))

    def test_modules_with_enterprise_enterprise_param(self):
        with mock.patch.object(config, "MODULES", ["enterprise"]):
            self.assertTrue(is_enterprise_param_available({"enterprise_only": True}))
