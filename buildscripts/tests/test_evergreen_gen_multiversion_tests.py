''' Tests for the multiversion generators '''

import os
import unittest
from tempfile import TemporaryDirectory, NamedTemporaryFile

from mock import patch, MagicMock
from click.testing import CliRunner

from buildscripts import evergreen_gen_multiversion_tests as under_test
import buildscripts.evergreen_generate_resmoke_tasks as generate_resmoke
from buildscripts.util.fileops import read_yaml_file

# pylint: disable=missing-docstring, no-self-use


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


class TestGenerateExcludeYaml(unittest.TestCase):
    def setUp(self):
        self._tmpdir = TemporaryDirectory()

    def tearDown(self):
        if self._tmpdir is not None:
            self._tmpdir.cleanup()

    def assert_contents(self, expected):
        actual = read_yaml_file(os.path.join(self._tmpdir.name, under_test.EXCLUDE_TAGS_FILE))
        self.assertEqual(actual, expected)

    def patch_and_run(self, latest, last_lts):
        """
        Helper to patch and run the test.
        """
        mock_multiversion_methods = {
            'get_backports_required_last_lts_hash': MagicMock(),
            'get_last_lts_yaml': MagicMock(return_value=last_lts)
        }

        with patch.multiple('buildscripts.evergreen_gen_multiversion_tests',
                            **mock_multiversion_methods):
            with patch('buildscripts.evergreen_gen_multiversion_tests.read_yaml_file',
                       return_value=latest) as mock_read_yaml:

                output = os.path.join(self._tmpdir.name, under_test.EXCLUDE_TAGS_FILE)
                runner = CliRunner()
                result = runner.invoke(
                    under_test.generate_exclude_yaml,
                    [f"--output={output}", '--task-path-suffix=/data/multiversion'])

                self.assertEqual(result.exit_code, 0, result)
                mock_read_yaml.assert_called_once()
                mock_multiversion_methods[
                    'get_backports_required_last_lts_hash'].assert_called_once()
                mock_multiversion_methods['get_last_lts_yaml'].assert_called_once()

    def test_create_yaml_suite1(self):
        latest_yaml = {
            'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}], 'suites': {
                'suite1': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'},
                           {'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
            }
        }

        last_lts_yaml = {
            'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}], 'suites': {
                'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
            }
        }

        expected = {
            'selector': {
                'js_test': {'jstests/fake_file1.js': ['suite1_backport_required_multiversion']}
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)

    def test_create_yaml_suite1_and_suite2(self):
        latest_yaml = {
            'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}], 'suites': {
                'suite1': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'},
                           {'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}],
                'suite2': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'}]
            }
        }

        last_lts_yaml = {
            'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}], 'suites': {
                'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
            }
        }

        expected = {
            'selector': {
                'js_test': {
                    'jstests/fake_file1.js': [
                        'suite1_backport_required_multiversion',
                        'suite2_backport_required_multiversion'
                    ]
                }
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)

    def test_both_all_are_none(self):
        latest_yaml = {
            'all': None, 'suites': {
                'suite1': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'},
                           {'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
            }
        }

        last_lts_yaml = {
            'all': None, 'suites': {
                'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
            }
        }

        expected = {
            'selector': {
                'js_test': {'jstests/fake_file1.js': ['suite1_backport_required_multiversion']}
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)

    def test_old_all_is_none(self):
        latest_yaml = {
            'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}], 'suites': {
                'suite1': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'},
                           {'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
            }
        }

        last_lts_yaml = {
            'all': None, 'suites': {
                'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
            }
        }

        expected = {
            'selector': {
                'js_test': {
                    'jstests/fake_file1.js': ['suite1_backport_required_multiversion'],
                    'jstests/fake_file0.js': ['backport_required_multiversion']
                }
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)

    def test_create_yaml_suite1_and_all(self):
        latest_yaml = {
            'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'},
                    {'ticket': 'fake_ticket4', 'test_file': 'jstests/fake_file4.js'}], 'suites': {
                        'suite1': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'},
                                   {'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
                    }
        }

        last_lts_yaml = {
            'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}], 'suites': {
                'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
            }
        }

        expected = {
            'selector': {
                'js_test': {
                    'jstests/fake_file1.js': ['suite1_backport_required_multiversion'],
                    'jstests/fake_file4.js': ['backport_required_multiversion']
                }
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)

    # Can delete after backporting the changed yml syntax.
    def test_not_backported(self):
        latest_yaml = {
            'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}], 'suites': {
                'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'},
                           {'ticket': 'fake_ticket3', 'test_file': 'jstests/fake_file3.js'}]
            }
        }

        last_lts_yaml = {
            'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
        }

        expected = {
            'selector': {
                'js_test': {
                    'jstests/fake_file0.js': ['backport_required_multiversion'],
                    'jstests/fake_file3.js': ['suite1_backport_required_multiversion']
                }
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)


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
