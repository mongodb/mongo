"""Module for retrieving the configuration of resmoke.py test suites."""
import collections
import copy
import os
from threading import Lock
from typing import Dict, List

import yaml

import buildscripts.resmokelib.utils.filesystem as fs
import buildscripts.resmokelib.logging.loggers as loggers
from buildscripts.resmokelib import config as _config, errors, utils
from buildscripts.resmokelib.testing import suite as _suite
from buildscripts.resmokelib.utils import load_yaml_file
from buildscripts.resmokelib.utils.dictionary import get_dict_value, merge_dicts, set_dict_value

SuiteName = str

_NAMED_SUITES = None


def get_named_suites() -> List[SuiteName]:
    """Return a list of the suites names."""
    global _NAMED_SUITES  # pylint: disable=global-statement

    if _NAMED_SUITES is None:
        # Skip "with_*server" and "no_server" because they do not define any test files to run.
        executor_only = {"with_server", "with_external_server", "no_server"}

        # Skip dbtest; its executable location needs to be updated for local usage.
        dbtest = {"dbtest"}

        explicit_suite_names = [
            name for name in ExplicitSuiteConfig.get_named_suites()
            if (name not in executor_only and name not in dbtest)
        ]
        composed_suite_names = MatrixSuiteConfig.get_named_suites()
        _NAMED_SUITES = explicit_suite_names + composed_suite_names
        _NAMED_SUITES.sort()
    return _NAMED_SUITES


def get_suite_files() -> Dict[str, str]:
    """Get the physical files defining these suites for parsing comments."""
    return merge_dicts(ExplicitSuiteConfig.get_suite_files(), MatrixSuiteConfig.get_suite_files())


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
    for suite_name in get_named_suites():
        try:
            suite = get_suite(suite_name)
            if test_kind and suite.get_test_kind_config() not in test_kind:
                continue

            for testfile in suite.tests:
                if isinstance(testfile, (dict, list)):
                    continue
                test_membership[testfile].append(suite_name)
        except IOError as err:
            # We ignore errors from missing files referenced in the test suite's "selector"
            # section. Certain test suites (e.g. unittests.yml) have a dedicated text file to
            # capture the list of tests they run; the text file may not be available if the
            # associated SCons target hasn't been built yet.
            if err.filename in _config.EXTERNAL_SUITE_SELECTORS:
                if not fail_on_missing_selector:
                    continue
            raise
    return test_membership


def get_suites(suite_names_or_paths, test_files):
    """Retrieve the Suite instances based on suite configuration files and override parameters.

    Args:
        suite_names_or_paths: A list of file paths pointing to suite YAML configuration files. For the suites
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
    for suite_filename in suite_names_or_paths:
        suite_config = _get_suite_config(suite_filename)
        suite = _suite.Suite(suite_filename, suite_config)
        if suite_roots:
            # Override the suite's default test files with those passed in from the command line.
            override_suite_config = suite_config.copy()
            override_suite_config.update(suite_roots)
            override_suite = _suite.Suite(suite_filename, override_suite_config)
            for test in override_suite.tests:
                if test in suite.excluded:
                    if _config.FORCE_EXCLUDED_TESTS:
                        loggers.ROOT_EXECUTOR_LOGGER.warning("Will forcibly run excluded test: %s",
                                                             test)
                    else:
                        raise errors.ResmokeError(f"'{test}' excluded in '{suite.get_name()}'")
            suite = override_suite
        suites.append(suite)
    return suites


def _make_suite_roots(files):
    return {"selector": {"roots": files}}


def _get_suite_config(suite_name_or_path):
    """Attempt to read YAML configuration from 'suite_path' for the suite."""
    return SuiteFinder.get_config_obj(suite_name_or_path)


def generate():
    MatrixSuiteConfig.generate_all_matrix_suite_files()


class SuiteConfigInterface:
    """Interface for suite configs."""

    @classmethod
    def get_config_obj(cls, suite_name):
        """Get the config object given the suite name, which can be a path."""
        pass

    @classmethod
    def get_named_suites(cls):
        """Populate the named suites by scanning `config_dir`."""
        pass

    @classmethod
    def get_suite_files(cls):
        """Get the physical files defining these suites for parsing comments."""
        pass


class ExplicitSuiteConfig(SuiteConfigInterface):
    """Class for storing the resmoke.py suite YAML configuration."""

    _name_suites_lock = Lock()
    _named_suites = {}

    @classmethod
    def get_config_obj(cls, suite_name):
        """Get the suite config object in the given file."""
        if suite_name in cls.get_named_suites():
            # Check if is a named suite first for efficiency.
            suite_path = cls.get_named_suites()[suite_name]
        elif fs.is_yaml_file(suite_name):
            # Check if is a path to a YAML file.
            if os.path.isfile(suite_name):
                suite_path = suite_name
            else:
                raise ValueError("Expected a suite YAML config, but got '%s'" % suite_name)
        else:
            # Not an explicit suite, return None.
            return None

        return utils.load_yaml_file(suite_path)

    @classmethod
    def get_named_suites(cls) -> Dict[str, str]:
        """Populate the named suites by scanning config_dir/suites."""
        with cls._name_suites_lock:
            if not cls._named_suites:
                suites_dir = os.path.join(_config.CONFIG_DIR, "suites")
                root = os.path.abspath(suites_dir)
                files = os.listdir(root)
                for filename in files:
                    (short_name, ext) = os.path.splitext(filename)
                    if ext in (".yml", ".yaml"):
                        pathname = os.path.join(root, filename)
                        cls._named_suites[short_name] = pathname

            return cls._named_suites

    @classmethod
    def get_suite_files(cls):
        """Get the suite files."""
        return cls.get_named_suites()


class MatrixSuiteConfig(SuiteConfigInterface):
    """Class for storing the resmoke.py suite YAML configuration."""

    _all_mappings = {}
    _all_overrides = {}

    @classmethod
    def get_suite_files(cls):
        """Get the suite files."""
        mappings_dir = os.path.join(cls.get_suites_dir(), "mappings")
        return cls.__get_suite_files_in_dir(mappings_dir)

    @classmethod
    def get_all_yamls(cls, target_dir):
        """Get all YAML files in the given directory."""
        return {
            short_name: load_yaml_file(path)
            for short_name, path in cls.__get_suite_files_in_dir(os.path.abspath(
                target_dir)).items()
        }

    @staticmethod
    def get_suites_dir():
        return os.path.join(_config.CONFIG_DIR, "matrix_suites")

    @classmethod
    def get_config_obj(cls, suite_name):
        """Get the suite config object in the given file and verify it matches the generated file."""

        config = cls._get_config_obj_no_verify(suite_name)

        if not config:
            return None

        generated_path = cls.get_generated_suite_path(suite_name)
        if not os.path.exists(generated_path):
            raise errors.InvalidMatrixSuiteError(
                f"No generated suite file was found for {suite_name}" +
                "To (re)generate the matrix suite files use `python3 buildscripts/resmoke.py generate-matrix-suites`"
            )

        new_text = cls.generate_matrix_suite_text(suite_name)
        with open(generated_path, "r") as file:
            old_text = file.read()
            if new_text != old_text:
                raise errors.InvalidMatrixSuiteError(
                    f"The generated file found on disk did not match the mapping file for {suite_name}. "
                    +
                    "To (re)generate the matrix suite files use `python3 buildscripts/resmoke.py generate-matrix-suites`"
                )

        return config

    @classmethod
    def _get_config_obj_no_verify(cls, suite_name):
        """Get the suite config object in the given file."""
        suites_dir = cls.get_suites_dir()
        matrix_suite = cls.parse_mappings_file(suites_dir, suite_name)
        if not matrix_suite:
            return None

        all_overrides = cls.parse_override_file(suites_dir)

        return cls.process_overrides(matrix_suite, all_overrides, suite_name)

    @classmethod
    def process_overrides(cls, suite, overrides, suite_name):
        """Provide override key-value pairs for a given matrix suite."""
        base_suite_name = suite["base_suite"]
        override_names = suite.get("overrides", None)
        excludes_names = suite.get("excludes", None)
        eval_names = suite.get("eval", None)
        description = suite.get("description")

        base_suite = ExplicitSuiteConfig.get_config_obj(base_suite_name)

        if base_suite is None:
            raise ValueError(f"Unknown base suite {base_suite_name} for matrix suite {suite_name}")

        res = copy.deepcopy(base_suite)
        res['matrix_suite'] = True
        overrides = copy.deepcopy(overrides)

        if description:
            res['description'] = description

        if override_names:
            for override_name in override_names:
                merge_dicts(res, overrides[override_name])

        if excludes_names:
            for excludes_name in excludes_names:
                excludes_dict = overrides[excludes_name]

                for key in excludes_dict:
                    if key not in ["exclude_with_any_tags", "exclude_files"]:
                        raise ValueError(f"{excludes_name}  is not supported in the 'excludes' tag")
                    value = excludes_dict[key]

                    if not isinstance(value, list):
                        raise ValueError(f"the {key} field must be a list")

                    base_value = get_dict_value(res, ["selector", key])

                    if base_value:
                        base_value.extend(value)
                        set_dict_value(res, ["selector", key], base_value)
                    else:
                        set_dict_value(res, ["selector", key], value)

        if eval_names:
            for eval_name in eval_names:
                eval_dict = overrides[eval_name]
                if len(eval_dict) != 1 or next(iter(eval_dict)) != "eval":
                    raise ValueError("Root key but be 'eval' for the eval tag.")

                value = eval_dict["eval"]

                if not isinstance(value, str):
                    raise ValueError("the eval field must be a list")

                path = ["executor", "config", "shell_options", "eval"]
                base_value = get_dict_value(res, path)

                if base_value:
                    set_dict_value(res, path, base_value + "; " + value)
                else:
                    set_dict_value(res, path, value)

        return res

    @classmethod
    def parse_override_file(cls, suites_dir):
        """Get a dictionary of all overrides in a given directory keyed by the suite name."""
        if not cls._all_overrides:
            overrides_dir = os.path.join(suites_dir, "overrides")
            overrides_files = cls.get_all_yamls(overrides_dir)

            for filename, override_config_file in overrides_files.items():
                for override_config in override_config_file:
                    if "name" in override_config and "value" in override_config:
                        cls._all_overrides[
                            f"{filename}.{override_config['name']}"] = override_config["value"]
                    else:
                        raise ValueError("Invalid override configuration, missing required keys. ",
                                         override_config)
        return cls._all_overrides

    @classmethod
    def parse_mappings_file(cls, suites_dir, suite_name):
        """Get the mapping object for a given suite name and directory to search for suite mappings."""
        all_matrix_suites = cls.get_all_mappings(suites_dir)

        if suite_name in all_matrix_suites.keys():
            return all_matrix_suites[suite_name]
        return None

    @classmethod
    def get_named_suites(cls):
        """Get a list of all suite names."""
        suites_dir = cls.get_suites_dir()
        all_mappings = cls.get_all_mappings(suites_dir)
        return list(all_mappings.keys())

    @classmethod
    def get_all_mappings(cls, suites_dir) -> Dict[str, str]:
        """Get a dictionary of all suite mapping files keyed by the suite name."""
        if not cls._all_mappings:
            mappings_dir = os.path.join(suites_dir, "mappings")
            mappings_files = cls.get_all_yamls(mappings_dir)

            for suite_name, suite_config in mappings_files.items():
                if "base_suite" in suite_config:
                    cls._all_mappings[suite_name] = suite_config
                else:
                    raise ValueError("Invalid suite configuration, missing required keys. ",
                                     suite_config)
        return cls._all_mappings

    @classmethod
    def __get_suite_files_in_dir(cls, target_dir):
        """Get the physical files defining these suites for parsing comments."""
        root = os.path.abspath(target_dir)
        files = os.listdir(root)
        all_files = {}
        for filename in files:
            (short_name, ext) = os.path.splitext(filename)
            if ext in (".yml", ".yaml"):
                all_files[short_name] = os.path.join(root, filename)

        return all_files

    @classmethod
    def get_generated_suite_path(cls, suite_name):
        matrix_dir = cls.get_suites_dir()
        suites_dir = os.path.join(matrix_dir, "generated_suites")
        if not os.path.exists(suites_dir):
            os.mkdir(suites_dir)
        path = os.path.join(suites_dir, f"{suite_name}.yml")
        return path

    @classmethod
    def generate_matrix_suite_text(cls, suite_name):
        suites_dir = cls.get_suites_dir()
        mappings_dir = os.path.join(suites_dir, "mappings")
        mapping_path = None
        for ext in (".yml", ".yaml"):
            path = os.path.join(mappings_dir, f"{suite_name}{ext}")
            if os.path.exists(path):
                mapping_path = path

        matrix_suite = cls._get_config_obj_no_verify(suite_name)

        if not matrix_suite or not mapping_path:
            print(f"Could not find mappings file for {suite_name}")
            return None

        yml = yaml.safe_dump(matrix_suite)
        comments = [
            "##########################################################",
            "# THIS IS A GENERATED FILE -- DO NOT MODIFY.",
            "# IF YOU WISH TO MODIFY THIS SUITE, MODIFY THE CORRESPONDING MATRIX SUITE MAPPING FILE",
            "# AND REGENERATE THE MATRIX SUITES.",
            "#",
            f"# matrix suite mapping file: {mapping_path}",
            "# regenerate matrix suites: buildscripts/resmoke.py generate-matrix-suites",
            "##########################################################",
        ]

        return "\n".join(comments) + "\n" + yml

    @classmethod
    def generate_matrix_suite_file(cls, suite_name):
        text = cls.generate_matrix_suite_text(suite_name)
        path = cls.get_generated_suite_path(suite_name)
        with open(path, 'w+') as file:
            file.write(text)
        print(f"Generated matrix suite file {path}")

    @classmethod
    def generate_all_matrix_suite_files(cls):
        suite_names = cls.get_named_suites()
        for suite_name in suite_names:
            cls.generate_matrix_suite_file(suite_name)


class SuiteFinder(object):
    """Utility/Factory class for getting polymorphic suite classes given a directory."""

    @staticmethod
    def get_config_obj(suite_path):
        """Get the suite config object in the given file."""
        explicit_suite = ExplicitSuiteConfig.get_config_obj(suite_path)
        matrix_suite = MatrixSuiteConfig.get_config_obj(suite_path)

        if not (explicit_suite or matrix_suite):
            raise errors.SuiteNotFound("Unknown suite '%s'" % suite_path)

        if explicit_suite and matrix_suite:
            raise errors.DuplicateSuiteDefinition(
                "Multiple definitions for suite '%s'" % suite_path)

        return matrix_suite or explicit_suite


def get_suite(suite_name_or_path) -> _suite.Suite:
    """Retrieve the Suite instance corresponding to a suite configuration file."""
    suite_config = _get_suite_config(suite_name_or_path)
    return _suite.Suite(suite_name_or_path, suite_config)
