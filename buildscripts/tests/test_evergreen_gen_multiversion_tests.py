''' Tests for the multiversion generators '''

import os
import shutil
import unittest
from tempfile import TemporaryDirectory, NamedTemporaryFile

from mock import patch, MagicMock
from shrub.v2 import BuildVariant, ShrubProject
from shrub.variant import DisplayTaskDefinition
from click.testing import CliRunner

from buildscripts import evergreen_gen_multiversion_tests as under_test
from buildscripts.evergreen_generate_resmoke_tasks import read_yaml
import buildscripts.evergreen_generate_resmoke_tasks as generate_resmoke

# pylint: disable=missing-docstring


class TestRun(unittest.TestCase):
    def setUp(self):
        self._tmpdir = TemporaryDirectory()

    def tearDown(self):
        self._tmpdir.cleanup()
        under_test.CONFIG_DIR = generate_resmoke.DEFAULT_CONFIG_VALUES

    @patch.object(under_test.EvergreenMultiversionConfigGenerator, 'generate_evg_tasks')
    @patch('buildscripts.evergreen_generate_resmoke_tasks.should_tasks_be_generated')
    @patch('buildscripts.evergreen_gen_multiversion_tests.write_file_to_dir')
    def test_empty_result_config_fails(self, generate_evg_tasks, should_tasks_be_generated,
                                       write_file_to_dir):
        # pylint: disable=unused-argument
        ''' Hijacks the write_file_to_dir function to prevent the configuration
        from being written to disk, and ensure the command fails '''
        under_test.CONFIG_DIR = self._tmpdir.name

        # NamedTemporaryFile doesn't work too well on Windows. We need to
        # close the fd's so that run_generate_tasks can open the files,
        # so we override the delete-on-close behaviour on Windows, and manually
        # handle cleanup later
        is_windows = os.name == 'nt'
        with NamedTemporaryFile(mode='w',
                                delete=not is_windows) as expansions_file, NamedTemporaryFile(
                                    mode='w', delete=not is_windows) as evg_conf:
            expansions_file.write(EXPANSIONS)
            expansions_file.flush()
            should_tasks_be_generated.return_value = True
            if is_windows:
                # on windows we need to close the fd's so that
                # run_generate_tasks can open the file handle
                expansions_file.close()
                evg_conf.close()

            runner = CliRunner()
            result = runner.invoke(
                under_test.run_generate_tasks,
                ['--expansion-file', expansions_file.name, '--evergreen-config', evg_conf.name])
            self.assertEqual(result.exit_code, 1, result)
            self.assertTrue(isinstance(result.exception, RuntimeError))
            self.assertEqual(
                str(result.exception),
                f"Multiversion suite generator unexpectedly yielded no configuration in '{self._tmpdir.name}'"
            )
            self.assertEqual(write_file_to_dir.call_count, 1)
            if is_windows:
                # on windows we need to manually delete these files, since
                # we've disabled the delete-on-close mechanics
                os.remove(expansions_file.name)
                os.remove(evg_conf.name)


class TestGenerateExcludeFiles(unittest.TestCase):
    def setUp(self):
        self._tmpdir = TemporaryDirectory()
        under_test.CONFIG_DIR = self._tmpdir.name

    def tearDown(self):
        if self._tmpdir is not None:
            self._tmpdir.cleanup()
        under_test.CONFIG_DIR = generate_resmoke.DEFAULT_CONFIG_VALUES["generated_config_dir"]

    def test_missing_dir_okay(self):
        self._tmpdir.cleanup()
        self._tmpdir = None
        self.assertFalse(os.path.exists(under_test.CONFIG_DIR))

        runner = CliRunner()
        result = runner.invoke(
            under_test.generate_exclude_yaml,
            ['--suite=test', '--task-path-suffix=test', '--is-generated-suite=true'])
        self.assertEqual(result.exit_code, 0, result)

    def test_empty_dir_okay(self):
        runner = CliRunner()
        result = runner.invoke(
            under_test.generate_exclude_yaml,
            ['--suite=test', '--task-path-suffix=test', '--is-generated-suite=true'])
        self.assertEqual(result.exit_code, 0, result)
        self.assertEqual(len(os.listdir(under_test.CONFIG_DIR)), 0)

    @patch('buildscripts.evergreen_gen_multiversion_tests.get_exclude_files')
    def test_adds_exclude_file(self, get_exclude_files):
        get_exclude_files.return_value = set()
        get_exclude_files.return_value.add('jstests/core/count_plan_summary.js')
        with open(self._tmpdir.name + "/sharding_jscore_passthrough_00.yml", mode='w') as fh:
            fh.write(CONF)

        runner = CliRunner()
        result = runner.invoke(
            under_test.generate_exclude_yaml,
            ['--suite=test', '--task-path-suffix=/data/multiversion', '--is-generated-suite=true'])
        self.assertEqual(result.exit_code, 0, result)
        self.assertEqual(get_exclude_files.call_count, 1)
        new_conf = read_yaml(self._tmpdir.name, "sharding_jscore_passthrough_00.yml")
        self.assertEqual(new_conf["selector"]["exclude_files"],
                         ["jstests/core/count_plan_summary.js"])


CONF = """# DO NOT EDIT THIS FILE. All manual edits will be lost.
# This file was generated by /data/mci/4d9a5adb3744dad3d44d93e5ff2a441f/src/buildscripts/evergreen_generate_resmoke_tasks.py from
# sharded_collections_jscore_passthrough.
executor:
  archive:
    hooks:
    - CheckReplDBHash
    - ValidateCollections
  config:
    shell_options:
      eval: load("jstests/libs/override_methods/implicitly_shard_accessed_collections.js")
      readMode: commands
  fixture:
    class: ShardedClusterFixture
    enable_balancer: false
    mongod_options:
      set_parameters:
        enableTestCommands: 1
    mongos_options:
      set_parameters:
        enableTestCommands: 1
    num_shards: 2
  hooks:
  - class: CheckReplDBHash
  - class: ValidateCollections
  - class: CleanEveryN
    n: 20
selector:
  exclude_with_any_tags:
  - assumes_against_mongod_not_mongos
  - assumes_no_implicit_collection_creation_after_drop
  - assumes_no_implicit_index_creation
  - assumes_unsharded_collection
  - cannot_create_unique_index_when_using_hashed_shard_key
  - requires_profiling
  roots:
  - jstests/core/count_plan_summary.js
  - jstests/core/json_schema/unique_items.js
  - jstests/core/arrayfind9.js
  - jstests/core/sort_numeric.js
  - jstests/core/remove4.js
  - jstests/core/aggregation_accepts_write_concern.js
  - jstests/core/geo3.js
  - jstests/core/minmax_edge.js
  - jstests/core/json_schema/additional_properties.js
  - jstests/core/update_min_max_examples.js
  - jstests/core/where2.js
  - jstests/core/geo_s2meridian.js
  - jstests/core/bittest.js
  - jstests/core/orj.js
  - jstests/core/find_dedup.js
  - jstests/core/hashed_index_queries.js
  - jstests/core/geo_circle2a.js
  - jstests/core/js4.js
  - jstests/core/index_create_with_nul_in_name.js
  - jstests/core/count_hint.js
  - jstests/core/sorta.js
  - jstests/core/orf.js
  - jstests/core/geo_s2within.js
  - jstests/core/json_schema/required.js
  - jstests/core/pop_server_13516.js
  - jstests/core/updatel.js
  - jstests/core/geo_s2descindex.js
test_kind: js_test
use_in_multiversion: sharded_collections_jscore_multiversion_passthrough
"""

EXPANSIONS = """task: t
build_variant: bv
fallback_num_sub_suites: 5
project: p
task_id: t0
task_name: t
use_multiversion: "true"
"""

if __name__ == '__main__':
    unittest.main()
