"""Module for retrieving the configuration of resmoke.py test suites."""

import collections
import copy
import itertools
import os
import pathlib
from threading import Lock
from typing import Any, Optional

import yaml

import buildscripts.resmokelib.utils.filesystem as fs
from buildscripts.resmokelib import bazel_suite_parser, errors, suite_hierarchy, utils
from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.logging import loggers
from buildscripts.resmokelib.testing import suite as _suite
from buildscripts.resmokelib.utils import load_yaml_file
from buildscripts.resmokelib.utils.dictionary import (
    extend_dict_lists,
    get_dict_value,
    merge_dicts,
    set_dict_value,
)
from buildscripts.resmokelib.utils.external_suite import make_external

SuiteName = str

_NAMED_SUITES = None


def get_named_suites() -> list[SuiteName]:
    """Return a list of the suites names."""
    global _NAMED_SUITES

    if _NAMED_SUITES is None:
        # Skip "with_*server" and "no_server" because they do not define any test files to run.
        executor_only = {"with_server", "with_external_server", "no_server"}

        # Skip dbtest; its executable location needs to be updated for local usage.
        dbtest = {"dbtest"}

        # skip config yaml files that might be in the same directory as suites
        misc_config_files = {"OWNERS"}

        skipped_files = executor_only.union(dbtest).union(misc_config_files)

        explicit_suite_names = [
            name for name in ExplicitSuiteConfig.get_named_suites() if (name not in skipped_files)
        ]
        composed_suite_names = MatrixSuiteConfig.get_named_suites()
        _NAMED_SUITES = explicit_suite_names + composed_suite_names
        _NAMED_SUITES.sort()
    return _NAMED_SUITES


def get_suite_files() -> dict[str, str]:
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
            # associated bazel target hasn't been built yet.
            if err.filename in _config.EXTERNAL_SUITE_SELECTORS:
                if not fail_on_missing_selector:
                    continue
            raise
        except KeyError as e:
            # For example, `test_kind` might be missing from their yaml config
            loggers.ROOT_EXECUTOR_LOGGER.error(
                "Error occurred while processing suite '%s': Field %s not found.", suite_name, e
            )
            raise
    return test_membership


def get_suites(suite_names_or_paths: list[str], test_files: list[str]) -> list[_suite.Suite]:
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
            for tag_filter in (_config.EXCLUDE_WITH_ANY_TAGS, _config.INCLUDE_WITH_ANY_TAGS)
        )
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
            # override_suite.tests is a list[list[str]] because of how the test_kind: parallel_fsm_workload_test
            # behaves, grouping the list of tests into a single group to run simultaneously. However, suite.excluded
            # is a list[str], and thus we need to flatten override_suite.tests to ensure we correctly check
            # whether each individual test is excluded.
            using_nested_test_suites = override_suite.tests and all(
                isinstance(item, list) for item in override_suite.tests
            )
            if using_nested_test_suites:
                flattened_tests = list(itertools.chain.from_iterable(override_suite.tests))
            else:
                flattened_tests = copy.deepcopy(override_suite.tests)
            for test in flattened_tests:
                if test in suite.excluded:
                    if _config.FORCE_EXCLUDED_TESTS:
                        loggers.ROOT_EXECUTOR_LOGGER.warning(
                            "Will forcibly run excluded test: %s", test
                        )
                    elif _config.SKIP_EXCLUDED_TESTS:
                        loggers.ROOT_EXECUTOR_LOGGER.warning(
                            "Will skip excluded test and continue with the suite execution: %s",
                            test,
                        )
                        if using_nested_test_suites:
                            for nested_list in override_suite.tests:
                                if test in nested_list:
                                    nested_list.remove(test)
                        else:
                            override_suite.tests.remove(test)
                    else:
                        raise errors.TestExcludedFromSuiteError(
                            f"'{test}' excluded in '{suite.get_name()}'"
                        )
            suite = override_suite

        if _config.SKIP_TESTS_COVERED_BY_MORE_COMPLEX_SUITES:
            if suite_roots:
                raise ValueError(
                    "Cannot use '--skipTestsCoveredByMoreComplexSuites' when tests have been passed in from the command line."
                )
            if _config.ORIGIN_SUITE:
                raise ValueError(
                    "Cannot use '--originSuite' with '--skipTestsCoveredByMoreComplexSuites'."
                )
            suite = _compute_minimal_test_set_suite(suite_filename)

        suites.append(suite)
    return suites


def _compute_minimal_test_set_suite(origin_suite_name):
    """
    Given a suite_A, returns the set of tests compatible with it, but incompatible
    with any suite_A_B more complex than it.

    The relationship of "more complex" is defined in suite_hierarchy.py.
    """

    # Compute the dag from the complexity graph
    dag = suite_hierarchy.compute_dag(suite_hierarchy.SUITE_HIERARCHY)

    # If we don't know how to compute the minimal test set because the suite's
    # information isn't present in the hierarchy, just return the suite as is.
    if origin_suite_name not in dag:
        suite_config = _get_suite_config(origin_suite_name)
        suite = _suite.Suite(origin_suite_name, suite_config)
        return suite

    # Build a dictionary of the tests in each suite.
    tests_in_suite = {}
    for suite_in_dag in dag.keys():
        suite_config = _get_suite_config(suite_in_dag)
        suite = _suite.Suite(suite_in_dag, suite_config)
        tests_in_suite[suite_in_dag] = set(suite.tests)

    tests = suite_hierarchy.compute_minimal_test_set(origin_suite_name, dag, tests_in_suite)
    min_suite_config = _get_suite_config(origin_suite_name).copy()
    min_suite_config.update(_make_suite_roots(list(tests)))
    return _suite.Suite(origin_suite_name, min_suite_config)


def _make_suite_roots(files):
    return {"selector": {"roots": files}}


def _get_suite_config(suite_name_or_path):
    """Attempt to read YAML configuration from 'suite_path' for the suite."""
    return SuiteFinder.get_config_obj_no_verify(suite_name_or_path)


def generate():
    MatrixSuiteConfig.generate_all_matrix_suite_files()


class SuiteConfigInterface:
    """Interface for suite configs."""

    @classmethod
    def get_config_obj_no_verify(cls, suite_name):
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


def expand_from_target_selector(config):
    from_target = config.get("selector", {}).get("from_target", {})
    if from_target:
        if len(config["selector"].keys()) > 1:
            # from_target is mutually exclusive with all other selector fields
            raise ValueError(
                f"'from_target' cannot be used with other selector fields. "
                f"Selector is: {config['selector']}"
            )

        # Parse the bazel target to extract selector configuration
        try:
            target_config = bazel_suite_parser.parse_resmoke_suite_test(from_target)
        except bazel_suite_parser.BazelParseError as err:
            raise ValueError(f"Failed to parse bazel target '{from_target}': {err}")

        # Resolve srcs to file paths
        resolved_roots = []
        for src_target in target_config["srcs"]:
            try:
                resolved_roots.append(bazel_suite_parser.resolve_target_to_files(src_target))
            except bazel_suite_parser.BazelParseError as err:
                raise ValueError(
                    f"Failed to resolve target '{src_target}' from '{from_target}': {err}"
                )

        # Resolve exclude_files to file paths
        resolved_exclude_files = []
        for exclude_target in target_config["exclude_files"]:
            try:
                resolved_exclude_files.append(
                    bazel_suite_parser.resolve_target_to_files(exclude_target)
                )
            except bazel_suite_parser.BazelParseError as err:
                raise ValueError(
                    f"Failed to resolve exclude target '{exclude_target}' from '{from_target}': {err}"
                )

        selector = {}
        if resolved_roots:
            selector["roots"] = resolved_roots
        if resolved_exclude_files:
            selector["exclude_files"] = resolved_exclude_files
        if target_config["include_with_any_tags"]:
            selector["include_with_any_tags"] = target_config["include_with_any_tags"]
        if target_config["exclude_with_any_tags"]:
            selector["exclude_with_any_tags"] = target_config["exclude_with_any_tags"]

        config["selector"] = selector

    return config


class ExplicitSuiteConfig(SuiteConfigInterface):
    """Class for storing the resmoke.py suite YAML configuration."""

    _name_suites_lock = Lock()
    _named_suites = {}

    @classmethod
    def get_config_obj_no_verify(cls, suite_name: str) -> dict:
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

        config = utils.load_yaml_file(suite_path)
        config = expand_from_target_selector(config)
        return config

    @classmethod
    def get_named_suites(cls) -> dict[str, str]:
        """Populate the named suites by scanning config_dir/suites."""
        with cls._name_suites_lock:
            if not cls._named_suites:
                suites_dirs = [
                    os.path.join(_config.CONFIG_DIR, "suites")
                ] + _config.MODULE_SUITE_DIRS
                for suites_dir in suites_dirs:
                    root = os.path.abspath(suites_dir)
                    if not os.path.exists(root):
                        continue
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
    def get_suite_files(cls) -> dict[str, str]:
        """Get the suite files from all matrix suite directories.

        Searches through all configured matrix suite directories (including module directories)
        and collects mapping files. Raises an error if duplicate suite names are found.

        Returns:
            Dictionary mapping suite names to their file paths.

        Raises:
            ValueError: If a suite name appears in multiple directories.
        """
        result = {}
        for suites_dir in cls.get_suites_dirs():
            mappings_dir = os.path.join(suites_dir, "mappings")
            for suite_name, path in cls.__get_suite_files_in_dir(mappings_dir).items():
                if suite_name in result:
                    raise ValueError(f"Duplicate matrix suite definition for {suite_name}")
                result[suite_name] = path
        return result

    @classmethod
    def get_all_yamls(cls, target_dir):
        """Get all YAML files in the given directory."""
        return {
            short_name: load_yaml_file(path)
            for short_name, path in cls.__get_suite_files_in_dir(
                os.path.abspath(target_dir)
            ).items()
        }

    @staticmethod
    def get_suites_dirs() -> list[str]:
        """Get all matrix suite directories to search for suite configurations.

        Returns a list of directories containing matrix suite definitions, including
        both the main resmoke configuration directory and any enabled module directories.

        Returns:
            List of absolute paths to matrix suite directories.
        """
        return [
            os.path.join(_config.CONFIG_DIR, "matrix_suites")
        ] + _config.MODULE_MATRIX_SUITE_DIRS

    @classmethod
    def get_config_obj_and_verify(cls, suite_name):
        """Get the suite config object in the given file and verify it matches the generated file."""

        config = cls.get_config_obj_no_verify(suite_name)

        if not config:
            return None

        generated_path = cls.get_generated_suite_path(suite_name)
        if not os.path.exists(generated_path):
            raise errors.InvalidMatrixSuiteError(
                f"No generated suite file was found for {suite_name}"
                + "To (re)generate the matrix suite files use `python3 buildscripts/resmoke.py generate-matrix-suites && bazel run //:format`"
            )

        new_text = cls.generate_matrix_suite_text(suite_name)
        new_yaml = yaml.safe_load(new_text)

        with open(generated_path, "r", encoding="utf8") as file:
            old_text = file.read()
            old_yaml = yaml.safe_load(old_text)
            if new_yaml != old_yaml:
                loggers.ROOT_EXECUTOR_LOGGER.error("Generated file on disk:")
                loggers.ROOT_EXECUTOR_LOGGER.error(old_text)
                loggers.ROOT_EXECUTOR_LOGGER.error("Generated text from mapping file:")
                loggers.ROOT_EXECUTOR_LOGGER.error(new_text)
                raise errors.InvalidMatrixSuiteError(
                    f"The generated file found on disk did not match the mapping file for {suite_name}. "
                    + "To (re)generate the matrix suite files use `python3 buildscripts/resmoke.py generate-matrix-suites && bazel run //:format`"
                )

        config = expand_from_target_selector(config)
        return config

    @classmethod
    def get_config_obj_no_verify(cls, suite_name):
        """Get the suite config object in the given file."""
        suites_dirs = cls.get_suites_dirs()
        if all(not os.path.exists(dir_path) for dir_path in suites_dirs):
            return None
        matrix_suite = cls.parse_mappings_file(suites_dirs, suite_name)
        if not matrix_suite:
            return None
        all_overrides = cls.parse_override_file(suites_dirs)

        config = cls.process_overrides(matrix_suite, all_overrides, suite_name)
        config = expand_from_target_selector(config)
        return config

    @classmethod
    def process_overrides(cls, suite, overrides, suite_name):
        """Provide override key-value pairs for a given matrix suite."""
        base_suite_name = suite["base_suite"]
        override_names = suite.get("overrides", None)
        excludes_names = suite.get("excludes", None)
        eval_names = suite.get("eval", None)
        extends_names = suite.get("extends", None)
        description = suite.get("description")

        base_suite = ExplicitSuiteConfig.get_config_obj_no_verify(base_suite_name)

        if base_suite is None:
            raise ValueError(f"Unknown base suite {base_suite_name} for matrix suite {suite_name}")

        res = copy.deepcopy(base_suite)
        res["matrix_suite"] = True
        overrides = copy.deepcopy(overrides)

        if description:
            res["description"] = description

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
                if eval_name in overrides:
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

        if extends_names:
            for extends_name in extends_names:
                extend_dict_lists(res, overrides[extends_name])

        return res

    @classmethod
    def parse_override_file(cls, suites_dirs: list[str]) -> dict[str, Any]:
        """Get a dictionary of all overrides from multiple directories keyed by override name.

        Parses override files from all provided suite directories and collects them into
        a single dictionary. Each override is keyed by "filename.override_name". Raises
        an error if duplicate override names are found across directories.

        Args:
            suites_dirs: List of directories containing matrix suite configurations.

        Returns:
            Dictionary mapping override keys (format: "filename.name") to their values.

        Raises:
            ValueError: If duplicate override definitions are found or if an override
                       configuration is missing required keys (name, value).
        """
        if not cls._all_overrides:
            for suites_dir in suites_dirs:
                overrides_dir = os.path.join(suites_dir, "overrides")
                overrides_files = cls.get_all_yamls(overrides_dir)

                for filename, override_config_file in overrides_files.items():
                    for override_config in override_config_file:
                        if "name" in override_config and "value" in override_config:
                            key = f"{filename}.{override_config['name']}"
                            if key in cls._all_overrides:
                                raise ValueError(f"Duplicate override definition for {key}")
                            cls._all_overrides[key] = override_config["value"]
                        else:
                            raise ValueError(
                                "Invalid override configuration, missing required keys. ",
                                override_config,
                            )
        return cls._all_overrides

    @classmethod
    def parse_mappings_file(
        cls, suites_dirs: list[str], suite_name: str
    ) -> Optional[dict[str, Any]]:
        """Get the mapping configuration for a given suite name.

        Looks up the suite in the aggregated mappings from all directories.

        Args:
            suites_dirs: List of directories containing matrix suite configurations.
            suite_name: Name of the suite to find.

        Returns:
            The suite mapping configuration dictionary if found, None otherwise.
        """
        all_matrix_suites = cls.get_all_mappings(suites_dirs)

        if suite_name in all_matrix_suites:
            return all_matrix_suites[suite_name]
        return None

    @classmethod
    def get_named_suites(cls):
        """Get a list of all suite names."""
        suites_dirs = cls.get_suites_dirs()
        all_mappings = cls.get_all_mappings(suites_dirs)
        return list(all_mappings.keys())

    @classmethod
    def get_all_mappings(cls, suites_dirs: list[str]) -> dict[str, Any]:
        """Get a dictionary of all suite mapping configurations from multiple directories.

        Collects and validates all matrix suite mapping files from the provided directories.
        Each mapping must contain a "base_suite" key. Raises an error if duplicate suite
        names are found across directories.

        Args:
            suites_dirs: List of directories containing matrix suite configurations.

        Returns:
            Dictionary mapping suite names to their configuration dictionaries.

        Raises:
            ValueError: If duplicate suite names are found or if a suite configuration
                       is missing the required "base_suite" key.
        """
        if not cls._all_mappings:
            for suites_dir in suites_dirs:
                mappings_dir = os.path.join(suites_dir, "mappings")
                mappings_files = cls.get_all_yamls(mappings_dir)

                for suite_name, suite_config in mappings_files.items():
                    if suite_name in cls._all_mappings:
                        raise ValueError(f"Duplicate matrix suite definition for {suite_name}")

                    if "base_suite" in suite_config:
                        cls._all_mappings[suite_name] = suite_config
                    else:
                        raise ValueError(
                            "Invalid suite configuration, missing required keys. ", suite_config
                        )
        return cls._all_mappings

    @classmethod
    def __get_suite_files_in_dir(cls, target_dir):
        """Get the physical files defining these suites for parsing comments."""
        root = os.path.abspath(target_dir)
        files = os.listdir(root)
        all_files = {}
        for filename in files:
            # As written, this function assumes that all 'yml' files are suite definitions. This
            # isn't true for OWNERS files, so we skip them.
            if filename == "OWNERS.yml":
                continue
            (short_name, ext) = os.path.splitext(filename)
            if ext in (".yml", ".yaml"):
                all_files[short_name] = os.path.join(root, filename)

        return all_files

    @classmethod
    def get_mappings_path(cls, suite_name: str) -> tuple[str, str]:
        """Get the mapping file path and suite directory for a given suite name.

        Searches all configured suite directories for a mapping file (.yml or .yaml)
        matching the given suite name. Ensures exactly one mapping file exists.

        Args:
            suite_name: Name of the suite to find the mapping file for.

        Returns:
            Tuple of (mapping_file_path, suite_directory_path).

        Raises:
            ValueError: If no mapping file is found or if multiple mapping files
                       exist for the same suite name across different directories.
        """
        suites_dirs = cls.get_suites_dirs()
        results = []
        for suites_dir in suites_dirs:
            mappings_dir = os.path.join(suites_dir, "mappings")
            for ext in (".yml", ".yaml"):
                path = os.path.join(mappings_dir, f"{suite_name}{ext}")
                if os.path.exists(path):
                    results.append((path, suites_dir))
        if len(results) == 0:
            raise ValueError(f"No mapping file found for suite {suite_name}")
        if len(results) > 1:
            raise ValueError(
                f"Multiple mapping files found for suite {suite_name}, cannot determine mapping file path: {results}"
            )
        return results[0]

    @classmethod
    def get_generated_suite_path(cls, suite_name: str) -> str:
        """Get the path where the generated suite file should be written.

        Determines the appropriate location for the generated suite file based on
        where the mapping file is located. Creates the generated_suites directory
        if it doesn't exist.

        Args:
            suite_name: Name of the suite to get the generated path for.

        Returns:
            Absolute path to the generated suite file.
        """
        _, suite_dir = cls.get_mappings_path(suite_name)

        suites_dir = os.path.join(suite_dir, "generated_suites")
        if not os.path.exists(suites_dir):
            os.mkdir(suites_dir)
        path = os.path.join(suites_dir, f"{suite_name}.yml")
        return path

    @classmethod
    def generate_matrix_suite_text(cls, suite_name: str) -> Optional[str]:
        """Generate the full text content for a matrix suite file.

        Creates the complete YAML content for a generated matrix suite file, including
        header comments indicating the file is auto-generated and referencing the
        source mapping file.

        Args:
            suite_name: Name of the suite to generate text for.

        Returns:
            Complete text content for the generated suite file, or None if the
            mapping file cannot be found.
        """
        mapping_path, _ = cls.get_mappings_path(suite_name)

        matrix_suite = cls.get_config_obj_no_verify(suite_name)

        if not matrix_suite or not mapping_path:
            print(f"Could not find mappings file for {suite_name}")
            return None

        # This path needs to output the same text on both windows and linux/mac
        mapping_path = pathlib.PurePath(mapping_path)
        yml = yaml.safe_dump(matrix_suite)
        comments = [
            "##########################################################",
            "# THIS IS A GENERATED FILE -- DO NOT MODIFY.",
            "# IF YOU WISH TO MODIFY THIS SUITE, MODIFY THE CORRESPONDING MATRIX SUITE MAPPING FILE",
            "# AND REGENERATE THE MATRIX SUITES.",
            "#",
            f"# matrix suite mapping file: {mapping_path.as_posix()}",
            "# regenerate matrix suites: buildscripts/resmoke.py generate-matrix-suites && bazel run //:format",
            "##########################################################",
        ]

        return "\n".join(comments) + "\n" + yml

    @classmethod
    def generate_matrix_suite_file(cls, suite_name):
        text = cls.generate_matrix_suite_text(suite_name)
        path = cls.get_generated_suite_path(suite_name)
        with open(path, "w+") as file:
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
    def get_config_obj_no_verify(suite_path):
        """Get the suite config object in the given file."""
        explicit_suite = ExplicitSuiteConfig.get_config_obj_no_verify(suite_path)
        matrix_suite = MatrixSuiteConfig.get_config_obj_no_verify(suite_path)

        suites = [item for item in [explicit_suite, matrix_suite] if item is not None]

        if len(suites) == 0:
            raise errors.SuiteNotFound("Unknown suite '%s'" % suite_path)
        elif len(suites) > 1:
            raise errors.DuplicateSuiteDefinition(
                "Multiple definitions for suite '%s'" % suite_path
            )

        suite = suites[0]

        # If this is running against an External System Under Test, we need to make the suite compatible.
        if _config.NOOP_MONGO_D_S_PROCESSES:
            make_external(suite)

        # Mutate the suite config as required by our own (resmokes) config. Pull this out into its own
        # function if it gets too big.
        if _config.FUZZ_RUNTIME_PARAMS:
            suite["executor"].setdefault("hooks", []).append({"class": "FuzzRuntimeParameters"})

        return suite


def get_suite(suite_name_or_path) -> _suite.Suite:
    """Retrieve the Suite instance corresponding to a suite configuration file."""
    return get_suites([suite_name_or_path], None)[0]
