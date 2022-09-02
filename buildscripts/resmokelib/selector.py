"""Test selection utility.

Defines filtering rules for what tests to include in a suite depending
on whether they apply to C++ unit tests, dbtests, or JS tests.
"""

import collections
import errno
import fnmatch
import os.path
import random
import subprocess
import sys

import buildscripts.resmokelib.testing.tags as _tags
from buildscripts.resmokelib import config
from buildscripts.resmokelib import errors
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.utils import globstar
from buildscripts.resmokelib.utils import jscomment

########################
#  Test file explorer  #
########################


class TestFileExplorer(object):
    """A component that can perform file system related operations.

    The file related code has been confined to this class for testability.
    """

    @staticmethod
    def is_glob_pattern(path):
        """Indicate if the provided path is a glob pattern.

        See buildscripts.resmokelib.utils.globstar.is_glob_pattern().
        """
        return globstar.is_glob_pattern(path)

    @staticmethod
    def iglob(pattern):  # noqa: D406,D407,D411,D413
        """Expand the given glob pattern with regard to the current working directory.

        See buildscripts.resmokelib.utils.globstar.iglob().
        Returns:
            A list of paths as a list(str).
        """
        return globstar.iglob(pattern)

    @staticmethod
    def jstest_tags(file_path):  # noqa: D406,D407,D411,D413
        """Extract the tags from a JavaScript test file.

        See buildscripts.resmokelib.utils.jscomment.get_tags().
        Returns:
            A list of tags.
        """
        return jscomment.get_tags(file_path)

    @staticmethod
    def read_root_file(root_file_path):  # noqa: D406,D407,D411,D413
        """Read a file containing the list of root test files.

        Args:
            root_file_path: the path to a file containing the path of each test on a separate line.
        Returns:
            A list of paths as a list(str).
        """
        tests = []
        with open(root_file_path, "r") as filep:
            for test_path in filep:
                test_path = test_path.strip()
                tests.append(test_path)
        return tests

    @staticmethod
    def fnmatchcase(name, pattern):
        """Indicate if the given name matches the given pattern.

        See buildscripts.resmokelib.utils.fnmatch.fnmatchcase().
        """
        return fnmatch.fnmatchcase(name, pattern)

    @staticmethod
    def isfile(path):
        """Indicate if the given path corresponds to an existing file."""
        return os.path.isfile(path)

    def list_dbtests(self, dbtest_binary):
        """List the available dbtests suites."""
        returncode, stdout, stderr = self._run_program(dbtest_binary, ["--list"])

        if returncode != 0:
            raise errors.ResmokeError("Getting list of dbtest suites failed"
                                      ", dbtest_binary=`{}`: stdout=`{}`, stderr=`{}`".format(
                                          dbtest_binary, stdout, stderr))
        return stdout.splitlines()

    @staticmethod
    def _run_program(binary, args):  # noqa: D406,D407,D411,D413
        """Run a program.

        Args:
            binary: the binary to run.
            args: a list of arguments for the binary.
        Returns:
            A tuple consisting of the program return code and its output.
        """
        command = [binary]
        command.extend(args)
        program = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout, stderr = program.communicate()
        return program.returncode, stdout.decode("utf-8"), stderr.decode("utf-8")

    @staticmethod
    def parse_tag_files(test_kind, tag_files=None, tagged_tests=None):
        """Parse the tag file and return a dict of tagged tests.

        The resulting dict will have as a key the filename and the
        value a list of tags, i.e., {'file1.js': ['tag1', 'tag2'], 'file2.js': ['tag2', 'tag3']}.
        """
        if tagged_tests is None:
            tagged_tests = collections.defaultdict(list)

        if tag_files is None:
            tag_files = []
        for tag_file in tag_files:
            if tag_file and os.path.exists(tag_file):
                tags_conf = _tags.TagsConfig.from_file(tag_file)
                tagged_roots = tags_conf.get_test_patterns(test_kind)
                for tagged_root in tagged_roots:
                    # Multiple tests could be returned for a set of tags.
                    tests = globstar.iglob(tagged_root)
                    test_tags = tags_conf.get_tags(test_kind, tagged_root)
                    for test in tests:
                        # A test could have a tag in more than one place, due to wildcards in the
                        # selector.
                        tagged_tests[test].extend(test_tags)
        return tagged_tests


class _TestList(object):
    """
    A list of tests on which filtering operations can be applied.

    Args:
        test_file_explorer: a TestFileExplorer instance.
        roots: a list of tests to initialize the _TestList with.
        tests_are_files: indicates if the tests are file paths. If so the _TestList will perform
            glob expansion of paths and check if they are existing files. If not, calling
            'include_files()' or 'exclude_files()' will raise an TypeError.
    """

    def __init__(self, test_file_explorer, roots, tests_are_files=True):
        """Initialize the _TestList with a TestFileExplorer component and a list of root tests."""
        self._test_file_explorer = test_file_explorer
        self._tests_are_files = tests_are_files
        self._roots = self._expand_files(roots) if tests_are_files else roots
        self._filtered = set(self._roots)

    def _expand_files(self, tests):
        expanded_tests = []
        for test in tests:
            if self._test_file_explorer.is_glob_pattern(test):
                expanded_tests.extend(self._test_file_explorer.iglob(test))
            else:
                if not self._test_file_explorer.isfile(test):
                    raise ValueError("Unrecognized test file: {}".format(test))
                expanded_tests.append(os.path.normpath(test))
        return expanded_tests

    def include_files(self, include_files, force=False):
        """Filter the test list so that it only includes files matching 'include_files'.

        Args:
            include_files: a list of paths or glob patterns that match the files to include.
            force: if True include the matching files that were previously excluded, otherwise only
                   include files that match and were not previously excluded from this _TestList.
        """
        if not self._tests_are_files:
            raise TypeError("_TestList does not contain files.")
        expanded_include_files = set()
        for path in include_files:
            if self._test_file_explorer.is_glob_pattern(path):
                expanded_include_files.update(set(self._test_file_explorer.iglob(path)))
            else:
                expanded_include_files.add(os.path.normpath(path))
        self._filtered = self._filtered & expanded_include_files
        if force:
            self._filtered |= set(self._roots) & expanded_include_files

    def exclude_files(self, exclude_files):  # noqa: D406,D407,D411,D413
        """Exclude from the test list the files that match elements from 'exclude_files'.

        Args:
            exclude_files: a list of paths or glob patterns that match the files to exclude.
        Raises:
            ValueError: if exclude_files contains a non-globbed path that does not correspond to
                an existing file.
        """
        if not self._tests_are_files:
            raise TypeError("_TestList does not contain files.")
        for path in exclude_files:
            if self._test_file_explorer.is_glob_pattern(path):
                paths = self._test_file_explorer.iglob(path)
                for expanded_path in paths:
                    self._filtered.discard(expanded_path)
            else:
                path = os.path.normpath(path)
                if path not in self._roots:
                    raise ValueError(
                        ("Excluded test file {} does not exist, perhaps it was renamed or removed"
                         " , and should be modified in, or removed from, the exclude_files list.".
                         format(path)))
                self._filtered.discard(path)

    def match_tag_expression(self, tag_expression, get_tags):
        """Filter the test list to only include tests that match the tag expression.

        Args:
            tag_expression: a callable object that takes a list of tags and indicate if the required
                condition is met by returning a boolean.
            get_tags: a callable object that takes a test and returns the corresponding list of
                tags.
        """
        self._filtered = {test for test in self._filtered if tag_expression(get_tags(test))}

    def include_any_pattern(self, patterns):
        """Filter the test list to only include tests that match any provided glob patterns."""

        def match(test):
            """Return True if 'test' matches a pattern."""
            for pattern in patterns:
                if test == pattern or fnmatch.fnmatchcase(test, pattern):
                    return True
            return False

        self._filtered = {test for test in self._filtered if match(test)}

    def get_tests(self):
        """Return the test list as a list(str).

        The tests are returned in the same order as they are found in the root tests.
        """
        tests = []
        excluded = []
        for test in self._roots:
            if test in self._filtered:
                if config.TEST_FILES or test not in tests:
                    # Allow duplicate tests if the tests were explicitly duplicated on the CLI invocation or replay file.
                    tests.append(test)
            elif test not in excluded:
                excluded.append(test)
        return tests, excluded


##############################
#  Tag matching expressions  #
##############################


class _AllOfExpression(object):
    """A tag matching expression that requires all child expressions to match."""

    def __init__(self, children):
        self.__children = children

    def __call__(self, file_tags):
        return all(child(file_tags) for child in self.__children)


class _AnyOfExpression(object):
    """A tag matching expression that requires at least one of the child expressions."""

    def __init__(self, children):
        self.__children = children

    def __call__(self, file_tags):
        return any(child(file_tags) for child in self.__children)


class _NotExpression(object):
    """A tag matching expression that matches if and only if the child expression does not match."""

    def __init__(self, child):
        self.__child = child

    def __call__(self, file_tags):
        return not self.__child(file_tags)


class _MatchExpression(object):
    """A tag matching expression that matches when a specific tag is present."""

    def __init__(self, tag):
        self.__tag = tag

    def __call__(self, file_tags):
        return self.__tag in file_tags


def make_expression(conf):
    """Create a tag matching expression from an expression configuration.

    The syntax for the expression configuration is:
    - expr: str_expr | dict_expr
    - str_expr: "<tagname>"
    - dict_expr: allof_expr | anyof_expr | not_expr
    - allof_expr: {"$allOf": [expr, ...]}
    - anyof_expr: {"$anyOf": [expr, ...]}
    - not_expr: {"$not": expr}
    """
    if isinstance(conf, str):
        return _MatchExpression(conf)
    elif isinstance(conf, dict):
        if len(conf) != 1:
            raise ValueError("Tag matching expressions should only contain one key")
        key = list(conf.keys())[0]
        value = conf[key]
        if key == "$allOf":
            return _AllOfExpression(_make_expression_list(value))
        elif key == "$anyOf":
            return _AnyOfExpression(_make_expression_list(value))
        elif key == "$not":
            return _NotExpression(make_expression(value))
    raise ValueError("Invalid tag matching expression: {}".format(conf))


def _make_expression_list(configs):
    return [make_expression(conf) for conf in configs]


####################
#  Test Selectors  #
####################


class _SelectorConfig(object):
    """Base object to represent the configuration for test selection."""

    def __init__(self, root=None, roots=None, include_files=None, exclude_files=None,
                 include_tags=None, exclude_tags=None, include_with_any_tags=None,
                 exclude_with_any_tags=None, tag_file=None):
        """Initialize the _SelectorConfig from the configuration elements.

        Args:
            root: the path to a file containing the list of root tests. Incompatible with 'roots'.
            roots: a list of root tests. Incompatible with 'root'.
            include_files: a list of paths or glob patterns the tests must be included in.
            exclude_files: a list of paths or glob patterns the tests must not be included in.
            include_tags: a str or dict representing a tag matching expression that the tags of the
                selected tests must match. Incompatible with 'exclude_tags'.
            exclude_tags: a str or dict representing a tag matching expression that the tags of the
                selected tests must not match. Incompatible with 'include_tags'.
            include_with_any_tags: a list of tags. All selected tests must have at least one them.
            exclude_with_any_tags: a list of tags. No selected tests can have any of them.
            tag_file: filename of a tag file associating tests to tags.
        """
        # Incompatible arguments check.
        if root and roots:
            raise ValueError("root and roots cannot be specified at the same time")
        if include_tags and exclude_tags:
            raise ValueError("include_tags and exclude_tags cannot be specified at the same time")
        self.root = root
        self.roots = roots
        self.tag_file = tag_file
        self.include_files = utils.default_if_none(include_files, [])
        self.exclude_files = utils.default_if_none(exclude_files, [])
        include_with_any_tags = self.__merge_lists(include_with_any_tags,
                                                   config.INCLUDE_WITH_ANY_TAGS)
        exclude_with_any_tags = self.__merge_lists(exclude_with_any_tags,
                                                   config.EXCLUDE_WITH_ANY_TAGS)

        # This is functionally similar to `include_tags` but contains a list of tags rather
        # than an expression.
        include_with_all_tags = config.INCLUDE_TAGS

        self.tags_expression = self.__make_tags_expression(
            include_tags, exclude_tags, include_with_any_tags, exclude_with_any_tags,
            include_with_all_tags)

    @staticmethod
    def __merge_lists(list_a, list_b):
        if list_a or list_b:
            if list_a is None:
                return set(list_b)
            elif list_b is None:
                return set(list_a)
            return set(list_a) | set(list_b)
        return None

    @staticmethod
    def __make_tags_expression(include_tags, exclude_tags, include_with_any_tags,
                               exclude_with_any_tags, include_with_all_tags):
        expressions = []
        if include_tags:
            expressions.append(make_expression(include_tags))
        if include_with_all_tags:
            include_with_all_tags_expr = make_expression({"$allOf": include_with_all_tags})
            expressions.append(include_with_all_tags_expr)
        elif exclude_tags:
            expressions.append(_NotExpression(make_expression(exclude_tags)))
        if include_with_any_tags:
            include_with_any_expr = make_expression({"$anyOf": include_with_any_tags})
            expressions.append(include_with_any_expr)
        if exclude_with_any_tags:
            exclude_with_any_expr = make_expression({"$not": {"$anyOf": exclude_with_any_tags}})
            expressions.append(exclude_with_any_expr)

        if expressions:
            return _AllOfExpression(expressions)
        return None


class _Selector(object):
    """Selection algorithm to select tests matching a selector configuration."""

    def __init__(self, test_file_explorer, tests_are_files=True):
        """Initialize the _Selector.

        Args:
            test_file_explorer: a TestFileExplorer instance.
            tests_are_files: whether tests are files.
        """
        self._test_file_explorer = test_file_explorer
        self._tests_are_files = tests_are_files

    def select(self, selector_config):  # noqa: D406,D407,D411,D413
        """Select the test files that match the given configuration.

        Args:
            selector_config: a _SelectorConfig instance.
        Returns:
            A tuple with the list of selected tests and the list of excluded tests.
        """

        # 1. Find the root files.
        if selector_config.roots is not None:
            roots = selector_config.roots
        else:
            roots = self._test_file_explorer.read_root_file(selector_config.root)

        # 2. Create a _TestList.
        test_list = _TestList(self._test_file_explorer, roots, self._tests_are_files)
        # 3. Apply the exclude_files.
        if self._tests_are_files and selector_config.exclude_files:
            test_list.exclude_files(selector_config.exclude_files)
        # 4. Apply the tag filters.
        if selector_config.tags_expression:
            test_list.match_tag_expression(selector_config.tags_expression, self.get_tags)
        # 5. Apply the include files last with force=True to take precedence over the tags.
        if self._tests_are_files and selector_config.include_files:
            test_list.include_files(selector_config.include_files, force=True)

        return self.sort_tests(*test_list.get_tests())

    @staticmethod
    def sort_tests(tests, excluded):
        """Sort the tests before returning them."""
        if config.ORDER_TESTS_BY_NAME:
            return sorted(tests, key=str.lower), sorted(excluded, key=str.lower)
        return tests, excluded

    @staticmethod
    def get_tags(test_file):  # pylint: disable=unused-argument
        """Retrieve the tags associated with the give test file."""
        return []


class _JSTestSelectorConfig(_SelectorConfig):
    """_SelectorConfig subclass for JavaScript tests."""

    def __init__(self, roots=None, include_files=None, exclude_files=None,
                 include_with_any_tags=None, exclude_with_any_tags=None, include_tags=None,
                 exclude_tags=None, tag_file=None):
        _SelectorConfig.__init__(
            self, roots=roots, include_files=include_files, exclude_files=exclude_files,
            include_with_any_tags=include_with_any_tags,
            exclude_with_any_tags=exclude_with_any_tags, include_tags=include_tags,
            exclude_tags=exclude_tags, tag_file=tag_file)


class _JSTestSelector(_Selector):
    """_Selector subclass for JavaScript tests."""

    def __init__(self, test_file_explorer):
        _Selector.__init__(self, test_file_explorer)
        self._tags = self._test_file_explorer.parse_tag_files("js_test", config.TAG_FILES)

    def select(self, selector_config):
        self._tags = self._test_file_explorer.parse_tag_files("js_test", [selector_config.tag_file],
                                                              self._tags)
        return _Selector.select(self, selector_config)

    def get_tags(self, test_file):
        """Return tags from test_file."""
        file_tags = self._test_file_explorer.jstest_tags(test_file)
        if test_file in self._tags:
            return list(set(file_tags) | set(self._tags[test_file]))
        return file_tags


class _MultiJSTestSelectorConfig(_JSTestSelectorConfig):
    """_SelectorConfig subclass for selecting groups of JavaScript tests."""

    def __init__(self, group_size=None, group_count_multiplier=1, **kwargs):
        """Init function.

        :param group_size: number of tests in each group.
        :param group_count_multiplier: number of times to schedule each workload, can be a decimal.
               E.g. 2.5 means half of the workloads get scheduled twice, and half get scheduled
               3 times.
        :param kwargs: arguments forwarded to the superclass.
        """
        _JSTestSelectorConfig.__init__(self, **kwargs)
        self.group_size = group_size
        self.group_count_multiplier = group_count_multiplier


class _MultiJSTestSelector(_JSTestSelector):
    """_Selector subclass for selecting one group of JavaScript tests at a time.

    Each group can include one or more tests.

    E.g. [[test1.js, test2.js], [test3.js, test4.js]].
    """

    def select(self, selector_config):
        """Select the tests as follows.

        1. Create a corpus of tests to group by concatenating shuffled lists of raw tests
           until we exceed "total_tests" number of tests.
        2. Slice the corpus into "group_size" lists, put these lists in "grouped_tests".
        """
        tests, excluded = _JSTestSelector.select(self, selector_config)

        group_size = selector_config.group_size
        multi = selector_config.group_count_multiplier

        # We use the group size as a sentinel to determine if the tests are coming from
        # the command line, in which case group_size would be undefined. For command line
        # tests, we assume the user is trying to repro a certain issue, so we group all
        # of the tests together.
        if group_size is None:
            multi = 1
            group_size = len(tests)

        grouped_tests = []

        start = 0
        corpus = tests[:]
        random.shuffle(corpus)

        num_groups = len(tests) * multi / group_size
        while len(grouped_tests) < num_groups:
            if start + group_size > len(corpus):
                recycled_tests = corpus[:start]
                random.shuffle(recycled_tests)
                corpus = corpus[start:] + recycled_tests
                start = 0
            grouped_tests.append(corpus[start:start + group_size])
            start += group_size
        return grouped_tests, excluded

    @staticmethod
    def sort_tests(tests, excluded):
        """There is no need to sort FSM test groups."""
        return tests, excluded


class _CppTestSelectorConfig(_SelectorConfig):
    """_SelectorConfig subclass for cpp_integration_test and cpp_unit_test tests."""

    def __init__(self, root=config.DEFAULT_INTEGRATION_TEST_LIST, roots=None, include_files=None,
                 exclude_files=None):
        """Initialize _CppTestSelectorConfig."""
        if roots:
            # The 'roots' argument is only present when tests are specified on the command line
            # and in that case they take precedence over the tests in the root file.
            _SelectorConfig.__init__(self, roots=roots, include_files=include_files,
                                     exclude_files=exclude_files)
        else:
            _SelectorConfig.__init__(self, root=root, include_files=include_files,
                                     exclude_files=exclude_files)


class _CppTestSelector(_Selector):
    """_Selector subclass for cpp_integration_test and cpp_unit_test tests."""

    def __init__(self, test_file_explorer):
        """Initialize _CppTestSelector."""
        _Selector.__init__(self, test_file_explorer)

    def select(self, selector_config):
        """Return selected tests."""
        if selector_config.roots:
            # Tests have been specified on the command line. We use them without additional
            # filtering.
            test_list = _TestList(self._test_file_explorer, selector_config.roots)
            return test_list.get_tests()
        return _Selector.select(self, selector_config)


class _DbTestSelectorConfig(_SelectorConfig):
    """_Selector config subclass for db_test tests."""

    def __init__(self, binary=None, roots=None, include_suites=None):
        """Initialize _DbTestSelectorConfig."""
        _SelectorConfig.__init__(self, roots=roots)
        self.include_suites = utils.default_if_none(include_suites, [])

        # Command line option overrides the YAML configuration.
        binary = utils.default_if_none(config.DBTEST_EXECUTABLE, binary)
        # Use the default if nothing specified.
        binary = utils.default_if_none(binary, config.DEFAULT_DBTEST_EXECUTABLE)
        # Ensure that executable files on Windows have a ".exe" extension.
        if sys.platform == "win32" and os.path.splitext(binary)[1] != ".exe":
            binary += ".exe"
        self.binary = binary


class _DbTestSelector(_Selector):
    """_Selector subclass for db_test tests."""

    def __init__(self, test_file_explorer):
        """Initialize _DbTestSelector."""
        _Selector.__init__(self, test_file_explorer, tests_are_files=False)

    def select(self, selector_config):
        """Return selected tests."""
        if selector_config.roots:
            roots = selector_config.roots
        else:
            if not self._test_file_explorer.isfile(selector_config.binary):
                raise IOError(errno.ENOENT, "File not found", selector_config.binary)
            roots = self._test_file_explorer.list_dbtests(selector_config.binary)

        if config.INCLUDE_WITH_ANY_TAGS:
            # The db_tests do not currently support tags so we always return an empty array when the
            # --includeWithAnyTags option is used.
            return [], roots

        if selector_config.roots:
            # Tests have been specified on the command line. We use them without additional
            # filtering.
            return selector_config.roots, []

        if not selector_config.include_suites:
            return roots, []

        test_files = _TestList(self._test_file_explorer, roots, tests_are_files=False)
        test_files.include_any_pattern(selector_config.include_suites)

        return test_files.get_tests()


class _FileBasedSelectorConfig(_SelectorConfig):
    """_SelectorConfig subclass for json_schema_test and mql_model_mongod_test tests."""

    def __init__(self, roots, include_files=None, exclude_files=None):
        """Initialize _FileBasedSelectorConfig."""
        _SelectorConfig.__init__(self, roots=roots, include_files=include_files,
                                 exclude_files=exclude_files)


class _SleepTestCaseSelectorConfig(_SelectorConfig):
    """_SelectorConfig subclass for sleep_test tests."""

    def __init__(self, roots):
        """Initialize _SleepTestCaseSelectorConfig."""
        _SelectorConfig.__init__(self, roots=roots)


class _SleepTestCaseSelector(_Selector):
    """_Selector subclass for sleep_test tests."""

    def __init__(self, test_file_explorer):
        """Initialize _SleepTestCaseSelector."""
        _Selector.__init__(self, test_file_explorer, tests_are_files=False)


class _PyTestCaseSelectorConfig(_SelectorConfig):
    """_SelectorConfig subclass for py_test tests."""

    def __init__(self, roots, include_files=None, exclude_files=None):
        _SelectorConfig.__init__(self, roots=roots, include_files=include_files,
                                 exclude_files=exclude_files)


class _GennylibTestCaseSelectorConfig(_SelectorConfig):
    """_SelectorConfig subclass for gennylib_test tests."""

    def __init__(self):
        """Initialize _GennylibTestCaseSelectorConfig."""
        _SelectorConfig.__init__(self, roots=["dummy-gennylib-test-roots"])


class _GennylibTestCaseSelector(_Selector):
    """_Selector subclass for gennylib_test tests."""

    def __init__(self, test_file_explorer):
        """Initialize _GennylibTestCaseSelector."""
        _Selector.__init__(self, test_file_explorer, tests_are_files=False)


##########################################
# Module entry point for filtering tests #
##########################################

_DEFAULT_TEST_FILE_EXPLORER = TestFileExplorer()

_SELECTOR_REGISTRY = {
    "cpp_integration_test": (_CppTestSelectorConfig, _CppTestSelector),
    "cpp_unit_test": (_CppTestSelectorConfig, _CppTestSelector),
    "benchmark_test": (_CppTestSelectorConfig, _CppTestSelector),
    "sdam_json_test": (_FileBasedSelectorConfig, _Selector),
    "server_selection_json_test": (_FileBasedSelectorConfig, _Selector),
    "db_test": (_DbTestSelectorConfig, _DbTestSelector),
    "fsm_workload_test": (_JSTestSelectorConfig, _JSTestSelector),
    "parallel_fsm_workload_test": (_MultiJSTestSelectorConfig, _MultiJSTestSelector),
    "json_schema_test": (_FileBasedSelectorConfig, _Selector),
    "js_test": (_JSTestSelectorConfig, _JSTestSelector),
    "all_versions_js_test": (_JSTestSelectorConfig, _JSTestSelector),
    "mql_model_haskell_test": (_FileBasedSelectorConfig, _Selector),
    "mql_model_mongod_test": (_FileBasedSelectorConfig, _Selector),
    "multi_stmt_txn_passthrough": (_JSTestSelectorConfig, _JSTestSelector),
    "py_test": (_PyTestCaseSelectorConfig, _Selector),
    "sleep_test": (_SleepTestCaseSelectorConfig, _SleepTestCaseSelector),
    "genny_test": (_FileBasedSelectorConfig, _Selector),
    "gennylib_test": (_GennylibTestCaseSelectorConfig, _GennylibTestCaseSelector),
    "cpp_libfuzzer_test": (_CppTestSelectorConfig, _CppTestSelector),
    "tla_plus_test": (_FileBasedSelectorConfig, _Selector),
}


def filter_tests(test_kind, selector_config, test_file_explorer=_DEFAULT_TEST_FILE_EXPLORER):
    """Filter the tests according to a specified configuration.

    Args:
        test_kind: the test kind, from _SELECTOR_REGISTRY.
        selector_config: a dict containing the selector configuration.
        test_file_explorer: the TestFileExplorer to use. Using a TestFileExplorer other than
        the default one should not be needed except for mocking purposes.
    """
    if test_kind not in _SELECTOR_REGISTRY:
        raise ValueError("Unknown test kind '{}'".format(test_kind))
    selector_config_class, selector_class = _SELECTOR_REGISTRY[test_kind]
    selector = selector_class(test_file_explorer)
    selector_config = selector_config_class(**selector_config)
    return selector.select(selector_config)
