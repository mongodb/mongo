"""Module for retrieving the configuration of resmoke.py test suites."""

import collections
import optparse
import os

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import errors
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing import suite as _suite
from buildscripts.resmokelib.utils import load_yaml_file


def get_named_suites():
    """Return a sorted list of the suites names."""
    # Skip "with_*server" and "no_server" because they do not define any test files to run.
    executor_only = {"with_server", "with_external_server", "no_server"}
    names = [name for name in _config.NAMED_SUITES.keys() if name not in executor_only]
    names += MatrixSuiteConfig.get_all_suite_names()
    names.sort()
    return names


def get_named_suites_with_root_level_key(root_level_key):
    """Return the suites that contain the given root_level_key and their values."""
    all_suite_names = get_named_suites()
    suites_to_return = []

    for suite in all_suite_names:
        suite_config = _get_suite_config(suite)
        if root_level_key in suite_config.keys() and suite_config[root_level_key]:
            suites_to_return.append(
                {"origin": suite, "multiversion_name": suite_config[root_level_key]})
    return suites_to_return


def create_test_membership_map(fail_on_missing_selector=False, test_kind=None):
    """Return a dict keyed by test name containing all of the suites that will run that test.

    If 'test_kind' is specified, then only the mappings for that kind of test are returned. Multiple
    kinds of tests can be specified as an iterable (e.g. a tuple or list). This function parses the
    definition of every available test suite, which is an expensive operation. It is therefore
    desirable for it to only ever be called once.
    """
    if test_kind is not None:
        if isinstance(test_kind, str):
            test_kind = [test_kind]

        test_kind = frozenset(test_kind)

    test_membership = collections.defaultdict(list)
    suite_names = get_named_suites()
    for suite_name in suite_names:
        try:
            suite_config = _get_suite_config(suite_name)
            if test_kind and suite_config.get("test_kind") not in test_kind:
                continue
            suite = _suite.Suite(suite_name, suite_config)
        except IOError as err:
            # We ignore errors from missing files referenced in the test suite's "selector"
            # section. Certain test suites (e.g. unittests.yml) have a dedicated text file to
            # capture the list of tests they run; the text file may not be available if the
            # associated SCons target hasn't been built yet.
            if err.filename in _config.EXTERNAL_SUITE_SELECTORS:
                if not fail_on_missing_selector:
                    continue
            raise

        for testfile in suite.tests:
            if isinstance(testfile, (dict, list)):
                continue
            test_membership[testfile].append(suite_name)
    return test_membership


def get_suites(suite_files, test_files):
    """Retrieve the Suite instances based on suite configuration files and override parameters.

    Args:
        suite_files: A list of file paths pointing to suite YAML configuration files. For the suites
            defined in 'buildscripts/resmokeconfig/suites/' and matrix suites, a shorthand name consisting
            of the filename without the extension can be used.
        test_files: A list of file paths pointing to test files overriding the roots for the suites.
    """
    suite_roots = None
    if test_files:
        # Do not change the execution order of the tests passed as args, unless a tag option is
        # specified. If an option is specified, then sort the tests for consistent execution order.
        _config.ORDER_TESTS_BY_NAME = any(
            tag_filter is not None
            for tag_filter in (_config.EXCLUDE_WITH_ANY_TAGS, _config.INCLUDE_WITH_ANY_TAGS))
        # Build configuration for list of files to run.
        suite_roots = _make_suite_roots(test_files)

    suites = []
    for suite_filename in suite_files:
        suite_config = _get_suite_config(suite_filename)
        if suite_roots:
            # Override the suite's default test files with those passed in from the command line.
            suite_config.update(suite_roots)
        suite = _suite.Suite(suite_filename, suite_config)
        suites.append(suite)
    return suites


def get_suite(suite_file):
    """Retrieve the Suite instance corresponding to a suite configuration file."""
    suite_config = _get_suite_config(suite_file)
    return _suite.Suite(suite_file, suite_config)


def _make_suite_roots(files):
    return {"selector": {"roots": files}}


def _get_suite_config(suite_path):
    """Attempt to read YAML configuration from 'suite_path' for the suite."""
    return SuiteFinder.get_config_obj(suite_path)


class SuiteConfigInterface(object):
    """Interface for suite configs."""

    def __init__(self, yaml_path=None):
        """Initialize the suite config interface."""
        self.yaml_path = yaml_path


class ExplicitSuiteConfig(SuiteConfigInterface):
    """Class for storing the resmoke.py suite YAML configuration."""

    @staticmethod
    def get_config_obj(pathname):
        """Get the suite config object in the given file."""
        # Named executors or suites are specified as the basename of the file, without the .yml
        # extension.
        if not utils.is_yaml_file(pathname) and not os.path.dirname(pathname):
            if pathname not in _config.NAMED_SUITES:  # pylint: disable=unsupported-membership-test
                # Expand 'pathname' to full path.
                return None
            pathname = _config.NAMED_SUITES[pathname]  # pylint: disable=unsubscriptable-object

        if not utils.is_yaml_file(pathname) or not os.path.isfile(pathname):
            raise optparse.OptionValueError("Expected a suite YAML config, but got '%s'" % pathname)
        return utils.load_yaml_file(pathname)


class MatrixSuiteConfig(SuiteConfigInterface):
    """Class for storing the resmoke.py suite YAML configuration."""

    @staticmethod
    def get_all_yamls(target_dir):
        """Get all YAML files in the given directory."""
        all_files = {}
        root = os.path.abspath(target_dir)
        files = os.listdir(root)

        for filename in files:
            (short_name, ext) = os.path.splitext(filename)
            if ext in (".yml", ".yaml"):
                pathname = os.path.join(root, filename)

                if not utils.is_yaml_file(pathname) or not os.path.isfile(pathname):
                    raise optparse.OptionValueError(
                        "Expected a suite YAML config, but got '%s'" % pathname)
                all_files[short_name] = load_yaml_file(pathname)
        return all_files

    @staticmethod
    def _get_suites_dir():
        return os.path.join(_config.CONFIG_DIR, "matrix_suites")

    @classmethod
    def get_config_obj(cls, suite_path):
        """Get the suite config object in the given file."""
        suites_dir = cls._get_suites_dir()
        matrix_suite = cls.parse_mappings_file(suites_dir, suite_path)
        if not matrix_suite:
            return None

        all_overrides = cls.parse_override_file(suites_dir)

        return cls.process_overrides(matrix_suite, all_overrides)

    @classmethod
    def process_overrides(cls, suite, overrides):
        """Provide override key-value pairs for a given matrix suite."""
        base_suite_name = suite["base_suite"]
        suite_name = suite["suite_name"]
        override_names = suite.get("overrides", None)

        base_suite = ExplicitSuiteConfig.get_config_obj(base_suite_name)

        if base_suite is None:
            # pylint: disable=too-many-format-args
            raise ValueError("Unknown base suite %s for matrix suite %s".format(
                base_suite_name, suite_name))

        res = base_suite.copy()

        if override_names:
            for override_name in override_names:
                cls.merge_dicts(res, overrides[override_name])

        return res

    @classmethod
    def parse_override_file(cls, suites_dir):
        """Get a dictionary of all overrides in a given directory keyed by the suite name."""
        overrides_dir = os.path.join(suites_dir, "overrides")
        overrides_files = cls.get_all_yamls(overrides_dir)

        all_overrides = {}
        for filename, override_config_file in overrides_files.items():
            for override_config in override_config_file:
                if "name" in override_config and "value" in override_config:
                    all_overrides[f"{filename}.{override_config['name']}"] = override_config[
                        "value"]
                else:
                    raise ValueError("Invalid override configuration, missing required keys. ",
                                     override_config)

        return all_overrides

    @classmethod
    def parse_mappings_file(cls, suites_dir, suite_name):
        """Get the mapping object for a given suite name and directory to search for suite mappings."""
        all_matrix_suites = cls.get_all_mappings(suites_dir)

        if suite_name in all_matrix_suites:
            return all_matrix_suites[suite_name]
        return None

    @classmethod
    def get_all_suite_names(cls):
        """Get a list of all suite names."""
        suites_dir = cls._get_suites_dir()
        all_mappings = cls.get_all_mappings(suites_dir)
        return all_mappings.keys()

    @classmethod
    def get_all_mappings(cls, suites_dir):
        """Get a dictionary of all suite mapping files keyed by the suite name."""
        mappings_dir = os.path.join(suites_dir, "mappings")
        mappings_files = cls.get_all_yamls(mappings_dir)

        all_matrix_suites = {}
        for _, suite_config_file in mappings_files.items():
            for suite_config in suite_config_file:
                if "suite_name" in suite_config and "base_suite" in suite_config:
                    all_matrix_suites[suite_config["suite_name"]] = suite_config
                else:
                    raise ValueError("Invalid suite configuration, missing required keys. ",
                                     suite_config)
        return all_matrix_suites

    @classmethod
    def merge_dicts(cls, dict1, dict2):
        """Recursively merges dict2 into dict1."""
        if not isinstance(dict1, dict) or not isinstance(dict2, dict):
            return dict2
        for k in dict2:
            if k in dict1:
                dict1[k] = cls.merge_dicts(dict1[k], dict2[k])
            else:
                dict1[k] = dict2[k]
        return dict1


class SuiteFinder(object):
    """Utility/Factory class for getting polymorphic suite classes given a directory."""

    @staticmethod
    def get_config_obj(suite_path):
        """Get the suite config object in the given file."""
        explicit_suite = ExplicitSuiteConfig.get_config_obj(suite_path)
        matrix_suite = MatrixSuiteConfig.get_config_obj(suite_path)

        if not (explicit_suite or matrix_suite):
            raise errors.SuiteNotFound("Unknown suite 's'" % suite_path)

        if explicit_suite and matrix_suite:
            raise errors.DuplicateSuiteDefinition(
                "Multiple definitions for suite '%s'" % suite_path)

        return matrix_suite or explicit_suite

    @staticmethod
    def get_named_suites(config_dir):
        """Populate the named suites by scanning config_dir/suites."""
        named_suites = {}

        suites_dir = os.path.join(config_dir, "suites")
        root = os.path.abspath(suites_dir)
        files = os.listdir(root)
        for filename in files:
            (short_name, ext) = os.path.splitext(filename)
            if ext in (".yml", ".yaml"):
                pathname = os.path.join(root, filename)
                # TODO: store named suite in an object
                named_suites[short_name] = pathname

        return named_suites
