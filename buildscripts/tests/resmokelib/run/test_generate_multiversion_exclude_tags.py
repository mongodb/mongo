"""Unit tests for buildscripts/resmokelib/run/generate_multiversion_exclude_tags.py."""

import os
import unittest
from subprocess import CalledProcessError
from tempfile import TemporaryDirectory

from mock import MagicMock, patch

from buildscripts.resmokelib.config import MultiversionOptions
from buildscripts.resmokelib.run import generate_multiversion_exclude_tags as under_test
from buildscripts.util.fileops import read_yaml_file

EXCLUDE_TAGS_FILE = "multiversion_exclude_tags.yml"
EXPANSIONS_FILE = "expansions.yml"


class TestGenerateExcludeYaml(unittest.TestCase):
    def setUp(self):
        self._tmpdir = TemporaryDirectory()

    def tearDown(self):
        if self._tmpdir is not None:
            self._tmpdir.cleanup()

    def assert_contents(self, expected):
        actual = read_yaml_file(os.path.join(self._tmpdir.name, EXCLUDE_TAGS_FILE))
        self.assertEqual(actual, expected)

    def patch_and_run(self, latest, old, old_bin_version):
        """Helper to patch and run the test."""
        mock_multiversion_methods = {
            "get_backports_required_hash": MagicMock(),
            "get_old_yaml": MagicMock(return_value=old),
        }

        with patch.multiple(
            "buildscripts.resmokelib.run.generate_multiversion_exclude_tags",
            **mock_multiversion_methods,
        ):
            with patch(
                "buildscripts.resmokelib.run.generate_multiversion_exclude_tags.read_yaml_file",
                return_value=latest,
            ) as mock_read_yaml:
                output = os.path.join(self._tmpdir.name, EXCLUDE_TAGS_FILE)
                under_test.generate_exclude_yaml(
                    old_bin_version=old_bin_version,
                    output=output,
                    logger=MagicMock(),
                )

                mock_read_yaml.assert_called_once()
                mock_multiversion_methods["get_backports_required_hash"].assert_called_once()
                mock_multiversion_methods["get_old_yaml"].assert_called_once()

    def test_create_yaml_suite1(self):
        latest_yaml = {
            "last-continuous": None,
            "last-lts": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [
                        {"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"},
                        {"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"},
                    ]
                },
            },
        }

        old_yaml = {
            "last-continuous": None,
            "last-lts": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [{"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"}]
                },
            },
        }

        expected = {
            "selector": {
                "js_test": {"jstests/fake_file1.js": ["suite1_backport_required_multiversion"]}
            }
        }

        self.patch_and_run(latest_yaml, old_yaml, MultiversionOptions.LAST_LTS)
        self.assert_contents(expected)

    def test_create_yaml_suite1_and_suite2(self):
        latest_yaml = {
            "last-continuous": None,
            "last-lts": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [
                        {"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"},
                        {"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"},
                    ],
                    "suite2": [{"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"}],
                },
            },
        }

        old_yaml = {
            "last-continuous": None,
            "last-lts": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [{"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"}]
                },
            },
        }

        expected = {
            "selector": {
                "js_test": {
                    "jstests/fake_file1.js": [
                        "suite1_backport_required_multiversion",
                        "suite2_backport_required_multiversion",
                    ]
                }
            }
        }

        self.patch_and_run(latest_yaml, old_yaml, MultiversionOptions.LAST_LTS)
        self.assert_contents(expected)

    def test_both_all_are_none(self):
        latest_yaml = {
            "last-continuous": None,
            "last-lts": {
                "all": None,
                "suites": {
                    "suite1": [
                        {"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"},
                        {"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"},
                    ]
                },
            },
        }

        old_yaml = {
            "last-continuous": None,
            "last-lts": {
                "all": None,
                "suites": {
                    "suite1": [{"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"}]
                },
            },
        }

        expected = {
            "selector": {
                "js_test": {"jstests/fake_file1.js": ["suite1_backport_required_multiversion"]}
            }
        }

        self.patch_and_run(latest_yaml, old_yaml, MultiversionOptions.LAST_LTS)
        self.assert_contents(expected)

    def test_old_all_is_none(self):
        latest_yaml = {
            "last-continuous": None,
            "last-lts": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [
                        {"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"},
                        {"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"},
                    ]
                },
            },
        }

        old_yaml = {
            "last-continuous": None,
            "last-lts": {
                "all": None,
                "suites": {
                    "suite1": [{"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"}]
                },
            },
        }

        expected = {
            "selector": {
                "js_test": {
                    "jstests/fake_file1.js": ["suite1_backport_required_multiversion"],
                    "jstests/fake_file0.js": ["backport_required_multiversion"],
                }
            }
        }

        self.patch_and_run(latest_yaml, old_yaml, MultiversionOptions.LAST_LTS)
        self.assert_contents(expected)

    def test_create_yaml_suite1_and_all(self):
        latest_yaml = {
            "last-continuous": None,
            "last-lts": {
                "all": [
                    {"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"},
                    {"ticket": "fake_ticket4", "test_file": "jstests/fake_file4.js"},
                ],
                "suites": {
                    "suite1": [
                        {"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"},
                        {"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"},
                    ]
                },
            },
        }

        old_yaml = {
            "last-continuous": None,
            "last-lts": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [{"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"}]
                },
            },
        }

        expected = {
            "selector": {
                "js_test": {
                    "jstests/fake_file1.js": ["suite1_backport_required_multiversion"],
                    "jstests/fake_file4.js": ["backport_required_multiversion"],
                }
            }
        }

        self.patch_and_run(latest_yaml, old_yaml, MultiversionOptions.LAST_LTS)
        self.assert_contents(expected)

    def test_last_continuous(self):
        latest_yaml = {
            "last-continuous": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [
                        {"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"},
                        {"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"},
                    ]
                },
            },
            "last-lts": None,
        }

        old_yaml = {
            "last-continuous": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [{"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"}]
                },
            },
            "last-lts": None,
        }

        expected = {
            "selector": {
                "js_test": {"jstests/fake_file1.js": ["suite1_backport_required_multiversion"]}
            }
        }

        self.patch_and_run(latest_yaml, old_yaml, MultiversionOptions.LAST_CONTINUOUS)
        self.assert_contents(expected)

    def test_old_last_continuous_is_empty(self):
        latest_yaml = {
            "last-continuous": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [
                        {"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"},
                        {"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"},
                    ]
                },
            },
            "last-lts": None,
        }

        old_yaml = {
            "last-continuous": {"all": None, "suites": {}},
            "last-lts": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [{"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"}]
                },
            },
        }

        expected = {
            "selector": {
                "js_test": {
                    "jstests/fake_file0.js": ["backport_required_multiversion"],
                    "jstests/fake_file1.js": ["suite1_backport_required_multiversion"],
                    "jstests/fake_file2.js": ["suite1_backport_required_multiversion"],
                }
            }
        }

        self.patch_and_run(latest_yaml, old_yaml, MultiversionOptions.LAST_CONTINUOUS)
        self.assert_contents(expected)

    # Can delete after backporting the changed yml syntax.
    def test_not_backported(self):
        latest_yaml = {
            "last-continuous": None,
            "last-lts": {
                "all": [
                    {"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"},
                    {"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"},
                ],
                "suites": {
                    "suite1": [
                        {"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"},
                        {"ticket": "fake_ticket3", "test_file": "jstests/fake_file3.js"},
                    ]
                },
            },
        }

        old_yaml = {
            "all": [{"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"}],
            "suites": {
                "suite1": [{"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"}]
            },
        }

        expected = {
            "selector": {
                "js_test": {
                    "jstests/fake_file0.js": ["backport_required_multiversion"],
                    "jstests/fake_file3.js": ["suite1_backport_required_multiversion"],
                }
            }
        }

        self.patch_and_run(latest_yaml, old_yaml, MultiversionOptions.LAST_LTS)
        self.assert_contents(expected)

    def test_last_patch(self):
        # last_patch is diffed against the last-patch section of the old binary's file, exactly like
        # last-lts/last-continuous: only the entry missing from the old file is excluded.
        latest_yaml = {
            "last-continuous": None,
            "last-lts": None,
            "last-patch": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [
                        {"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"},
                        {"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"},
                    ]
                },
            },
        }

        old_yaml = {
            "last-continuous": None,
            "last-lts": None,
            "last-patch": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [{"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"}]
                },
            },
        }

        expected = {
            "selector": {
                "js_test": {"jstests/fake_file1.js": ["suite1_backport_required_multiversion"]}
            }
        }

        self.patch_and_run(latest_yaml, old_yaml, MultiversionOptions.LAST_PATCH)
        self.assert_contents(expected)

    def test_last_patch_section_missing_from_old(self):
        # Until this change is backported, the old binary's file has no `last-patch` section at all.
        # Every current last-patch entry should then be excluded (treat old exclusions as empty).
        latest_yaml = {
            "last-continuous": None,
            "last-lts": None,
            "last-patch": {
                "all": [{"ticket": "fake_ticket0", "test_file": "jstests/fake_file0.js"}],
                "suites": {
                    "suite1": [
                        {"ticket": "fake_ticket1", "test_file": "jstests/fake_file1.js"},
                        {"ticket": "fake_ticket2", "test_file": "jstests/fake_file2.js"},
                    ]
                },
            },
        }

        old_yaml = {
            "last-continuous": None,
            "last-lts": None,
        }

        expected = {
            "selector": {
                "js_test": {
                    "jstests/fake_file0.js": ["backport_required_multiversion"],
                    "jstests/fake_file1.js": ["suite1_backport_required_multiversion"],
                    "jstests/fake_file2.js": ["suite1_backport_required_multiversion"],
                }
            }
        }

        self.patch_and_run(latest_yaml, old_yaml, MultiversionOptions.LAST_PATCH)
        self.assert_contents(expected)


class TestGetGitFileContent(unittest.TestCase):
    COMMIT = "7aba7e33c0a0dcbff28618f506b689dd8f931ba8"

    @staticmethod
    def _show_failure():
        return CalledProcessError(128, ["git", "show"], stderr="fatal: bad object\n")

    @staticmethod
    def _show_promisor_failure():
        # `git show` in a partial clone surfaces the promisor remote's network error.
        return CalledProcessError(
            128,
            ["git", "show"],
            stderr="fatal: unable to access 'https://github.com/10gen/mongo.git/': "
            "The requested URL returned error: 503\n",
        )

    @staticmethod
    def _fetch_failure():
        return CalledProcessError(
            128, ["git", "fetch"], stderr="fatal: unable to access: error: 503\n"
        )

    def test_no_fetch_needed(self):
        with patch.object(
            under_test.subprocess, "run", return_value=MagicMock(stdout="contents")
        ) as mock_run:
            self.assertEqual(under_test.get_git_file_content(self.COMMIT, MagicMock()), "contents")
            self.assertEqual(mock_run.call_count, 1)

    def test_transient_fetch_failure_is_retried(self):
        # `git show` fails, the first fetch hits a 503, and the retried fetch succeeds.
        side_effects = [
            self._show_failure(),
            self._fetch_failure(),
            MagicMock(),  # successful fetch
            MagicMock(stdout="contents"),  # successful show
        ]
        with patch.object(under_test.subprocess, "run", side_effect=side_effects) as mock_run:
            with patch("tenacity.nap.time.sleep") as mock_sleep:
                self.assertEqual(
                    under_test.get_git_file_content(self.COMMIT, MagicMock()), "contents"
                )
        self.assertEqual(mock_run.call_count, len(side_effects))
        mock_sleep.assert_called_once()

    def test_persistent_fetch_failure_raises_after_max_attempts(self):
        side_effects = [self._show_failure()] + [
            self._fetch_failure() for _ in range(under_test.FETCH_MAX_ATTEMPTS)
        ]
        with patch.object(under_test.subprocess, "run", side_effect=side_effects) as mock_run:
            with patch("tenacity.nap.time.sleep") as mock_sleep:
                with self.assertRaisesRegex(RuntimeError, "503"):
                    under_test.get_git_file_content(self.COMMIT, MagicMock())
        self.assertEqual(mock_run.call_count, len(side_effects))
        # We back off between attempts, but not after the final one.
        self.assertEqual(mock_sleep.call_count, under_test.FETCH_MAX_ATTEMPTS - 1)

    def test_transient_show_failure_is_retried(self):
        # The BF-45380 scenario: the fetch succeeds, but `git show` still hits a 503 because
        # Evergreen's partial clone makes it lazily fetch the blob from the promisor remote.
        # The retry has to cover `git show`, not just the fetch.
        side_effects = [
            self._show_failure(),
            MagicMock(),  # successful fetch
            self._show_promisor_failure(),
            MagicMock(),  # successful fetch
            MagicMock(stdout="contents"),  # successful show
        ]
        with patch.object(under_test.subprocess, "run", side_effect=side_effects) as mock_run:
            with patch("tenacity.nap.time.sleep") as mock_sleep:
                self.assertEqual(
                    under_test.get_git_file_content(self.COMMIT, MagicMock()), "contents"
                )
        self.assertEqual(mock_run.call_count, len(side_effects))
        mock_sleep.assert_called_once()


if __name__ == "__main__":
    unittest.main()
