"""
Test selection utility.

Defines filtering rules for what tests to include in a suite depending
on whether they apply to C++ unit tests, dbtests, or JS tests.
"""

from __future__ import absolute_import

import collections
import errno
import fnmatch
import os.path
import subprocess
import sys

import buildscripts.ciconfig.tags as _tags
from . import config
from . import errors
from . import utils
from .utils import globstar
from .utils import jscomment


########################
#  Test file explorer  #
########################


class TestFileExplorer(object):
    """A component that can perform file system related operations.

    The file related code has been confined to this class for testability.
    """
    def is_glob_pattern(self, path):
        """Indicates if the provided path is a glob pattern.

        See buildscripts.resmokelib.utils.globstar.is_glob_pattern().
        """
        return globstar.is_glob_pattern(path)

    def iglob(self, pattern):
        """Expands the given glob pattern with regard to the current working directory.

        See buildscripts.resmokelib.utils.globstar.iglob().
        Returns:
            A list of paths as a list(str).
        """
        return globstar.iglob(pattern)

    def jstest_tags(self, file_path):
        """Extracts the tags from a JavaScript test file.

        See buildscripts.resmokelib.utils.jscomment.get_tags().
        Returns:
            A list of tags.
        """
        return jscomment.get_tags(file_path)

    def read_root_file(self, root_file_path):
        """Reads a file containing the list of root test files.

        Args:
            root_file_path: the path to a file containing the path of each test on a separate line.
        Returns:
            A list of paths as a list(str).
        """
        tests = []
        with open(root_file_path, "rb") as filep:
            for test_path in filep:
                test_path = test_path.strip()
                tests.append(test_path)
        return tests

    def fnmatchcase(self, name, pattern):
        """Indicates if the given name matches the given pattern.

        See buildscripts.resmokelib.utils.fnmatch.fnmatchcase().
        """
        return fnmatch.fnmatchcase(name, pattern)

    def isfile(self, path):
        """Indicates if the given path corresponds to an existing file."""
        return os.path.isfile(path)

    def list_dbtests(self, dbtest_binary):
        """Lists the available dbtests suites."""
        returncode, stdout = self._run_program(dbtest_binary, ["--list"])

        if returncode != 0:
            raise errors.ResmokeError("Getting list of dbtest suites failed")

        return stdout.splitlines()

    def _run_program(self, binary, args):
        """Runs a program.

        Args:
            binary: the binary to run.
            args: a list of arguments for the binary.
        Returns:
            A tuple consisting of the program return code and its output.
        """
        command = [binary]
        command.extend(args)
        program = subprocess.Popen(command, stdout=subprocess.PIPE)
        stdout = program.communicate()[0]

        return program.returncode, stdout

    def parse_tag_file(self, test_kind):
        """
        Parses the tag file and return a dict of tagged tests, with the key the filename and the
        value a list of tags, i.e., {'file1.js': ['tag1', 'tag2'], 'file2.js': ['tag2', 'tag3']}.
        """
        tagged_tests = collections.defaultdict(list)
        if config.TAG_FILE:
            tags_conf = _tags.TagsConfig.from_file(config.TAG_FILE)
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
        """Initializes the _TestList with a TestFileExplorer component and a list of root tests."""
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
                expanded_tests.append(test)
        return expanded_tests

    def include_files(self, include_files, force=False):
        """Filters the test list so that it only includes files matching 'include_files'.

         Args:
             include_files: a list of paths or glob patterns that match the files to include.
             force: if True include the matching files that were previously excluded, otherwise
                only include files that match and were not previously excluded from this _TestList.
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

    def exclude_files(self, exclude_files):
        """Excludes from the test list the files that match elements from 'exclude_files'.

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
                    raise ValueError("Unrecognized test file: {}".format(path))
                self._filtered.discard(path)

    def match_tag_expression(self, tag_expression, get_tags):
        """Filters the test list to only include tests that match the tag expression.

        Args:
            tag_expression: a callable object that takes a list of tags and indicate if the required
                condition is met by returning a boolean.
            get_tags: a callable object that takes a test and returns the corresponding list of
                tags.
        """
        self._filtered = {test for test in self._filtered
                          if tag_expression(get_tags(test))}

    def include_any_pattern(self, patterns):
        """
        Filters the test list to only include tests that match any of the given glob patterns.
        """
        def match(test):
            for pattern in patterns:
                if test == pattern or fnmatch.fnmatchcase(test, pattern):
                    return True
            return False

        self._filtered = {test for test in self._filtered if match(test)}

    def get_tests(self):
        """
        Returns the test list as a list(str).

        The tests are returned in the same order as they are found in the root tests.
        """
        tests = []
        for test in self._roots:
            if test in self._filtered and test not in tests:
                tests.append(test)
        return tests


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
    """Creates a tag matching expression from an expression configuration.

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
        key = conf.keys()[0]
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
    def __init__(self, root=None, roots=None,
                 include_files=None, exclude_files=None,
                 include_tags=None, exclude_tags=None,
                 include_with_any_tags=None, exclude_with_any_tags=None):
        """
        Initializes the _SelectorConfig from the configuration elements.

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
        """
        # Incompatible arguments check.
        if root and roots:
            raise ValueError("root and roots cannot be specified at the same time")
        if include_tags and exclude_tags:
            raise ValueError("include_tags and exclude_tags cannot be specified at the same time")
        self.root = root
        self.roots = roots
        self.include_files = utils.default_if_none(include_files, [])
        self.exclude_files = utils.default_if_none(exclude_files, [])
        include_with_any_tags = self.__merge_lists(include_with_any_tags,
                                                   config.INCLUDE_WITH_ANY_TAGS)
        exclude_with_any_tags = self.__merge_lists(exclude_with_any_tags,
                                                   config.EXCLUDE_WITH_ANY_TAGS)

        self.tags_expression = self.__make_tags_expression(include_tags,
                                                           exclude_tags,
                                                           include_with_any_tags,
                                                           exclude_with_any_tags)

    @staticmethod
    def __merge_lists(list_a, list_b):
        if list_a or list_b:
            if list_a is None:
                return set(list_b)
            elif list_b is None:
                return set(list_a)
            else:
                return set(list_a) | set(list_b)
        else:
            return None

    @staticmethod
    def __make_tags_expression(include_tags, exclude_tags,
                               include_with_any_tags, exclude_with_any_tags):
        expressions = []
        if include_tags:
            expressions.append(make_expression(include_tags))
        elif exclude_tags:
            expressions.append(_NotExpression(make_expression(exclude_tags)))
        if include_with_any_tags:
            include_with_any_expr = make_expression(
                {"$anyOf": include_with_any_tags})
            expressions.append(include_with_any_expr)
        if exclude_with_any_tags:
            exclude_with_any_expr = make_expression(
                {"$not": {"$anyOf": exclude_with_any_tags}})
            expressions.append(exclude_with_any_expr)

        if expressions:
            return _AllOfExpression(expressions)
        else:
            return None


class _Selector(object):
    """Selection algorithm to select tests matching a selector configuration."""
    def __init__(self, test_file_explorer, tests_are_files=True):
        """
        Initializes the _Selector.

        Args:
            test_file_explorer: a TestFileExplorer instance.
        """
        self._test_file_explorer = test_file_explorer
        self._tests_are_files = tests_are_files

    def select(self, selector_config):
        """Select the test files that match the given configuration.

        Args:
            selector_config: a _SelectorConfig instance.
        """

        # 1. Find the root files.
        if selector_config.roots:
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
        return test_list.get_tests()

    def get_tags(self, test_file):
        """Retrieves the tags associated with the give test file."""
        return []


class _JSTestSelectorConfig(_SelectorConfig):
    """_SelectorConfig subclass for js_test tests."""
    def __init__(self, roots=None,
                 include_files=None, exclude_files=None,
                 include_with_any_tags=None, exclude_with_any_tags=None,
                 include_tags=None, exclude_tags=None):
        _SelectorConfig.__init__(self, roots=roots,
                                 include_files=include_files, exclude_files=exclude_files,
                                 include_with_any_tags=include_with_any_tags,
                                 exclude_with_any_tags=exclude_with_any_tags,
                                 include_tags=include_tags, exclude_tags=exclude_tags)


class _JSTestSelector(_Selector):
    """_Selector subclass for js_test tests."""
    def __init__(self, test_file_explorer):
        _Selector.__init__(self, test_file_explorer)
        self._tags = self._test_file_explorer.parse_tag_file("js_test")

    def get_tags(self, test_file):
        file_tags = self._test_file_explorer.jstest_tags(test_file)
        if test_file in self._tags:
            return list(set(file_tags) | set(self._tags[test_file]))
        return file_tags


class _CppTestSelectorConfig(_SelectorConfig):
    """_SelectorConfig subclass for cpp_integration_test and cpp_unit_test tests."""
    def __init__(self, root=config.DEFAULT_INTEGRATION_TEST_LIST, roots=None,
                 include_files=None, exclude_files=None):
        if roots:
            # The 'roots' argument is only present when tests are specified on the command line
            # and in that case they take precedence over the tests in the root file.
            _SelectorConfig.__init__(self, roots=roots,
                                     include_files=include_files, exclude_files=exclude_files)
        else:
            _SelectorConfig.__init__(self, root=root,
                                     include_files=include_files, exclude_files=exclude_files)


class _CppTestSelector(_Selector):
    """_Selector subclass for cpp_integration_test and cpp_unit_test tests."""
    def __init__(self, test_file_explorer):
        _Selector.__init__(self, test_file_explorer)

    def select(self, selector_config):
        if selector_config.roots:
            # Tests have been specified on the command line. We use them without additional
            # filtering.
            test_list = _TestList(self._test_file_explorer, selector_config.roots)
            return test_list.get_tests()
        return _Selector.select(self, selector_config)


class _DbTestSelectorConfig(_SelectorConfig):
    """_Selector config subclass for db_test tests."""
    def __init__(self, binary=None, roots=None, include_suites=None):
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
        _Selector.__init__(self, test_file_explorer, tests_are_files=False)

    def select(self, selector_config):
        if config.INCLUDE_WITH_ANY_TAGS:
            # The db_tests do not currently support tags so we always return an empty array when the
            # --includeWithAnyTags option is used.
            return []

        if selector_config.roots:
            # Tests have been specified on the command line. We use them without additional
            # filtering.
            return selector_config.roots

        if not self._test_file_explorer.isfile(selector_config.binary):
            raise IOError(errno.ENOENT, "File not found", selector_config.binary)

        dbtests = self._test_file_explorer.list_dbtests(selector_config.binary)

        if not selector_config.include_suites:
            return dbtests

        test_files = _TestList(self._test_file_explorer, dbtests, tests_are_files=False)
        test_files.include_any_pattern(selector_config.include_suites)

        return test_files.get_tests()


class _JsonSchemaTestSelectorConfig(_SelectorConfig):
    """_SelectorConfig subclass for json_schema_test tests."""
    def __init__(self, roots, include_files=None, exclude_files=None):
        _SelectorConfig.__init__(self, roots=roots,
                                 include_files=include_files, exclude_files=exclude_files)


##########################################
# Module entry point for filtering tests #
##########################################

_DEFAULT_TEST_FILE_EXPLORER = TestFileExplorer()


_SELECTOR_REGISTRY = {
    "cpp_integration_test": (_CppTestSelectorConfig, _CppTestSelector),
    "cpp_unit_test": (_CppTestSelectorConfig, _CppTestSelector),
    "db_test": (_DbTestSelectorConfig, _DbTestSelector),
    "json_schema_test": (_JsonSchemaTestSelectorConfig, _Selector),
    "js_test": (_JSTestSelectorConfig, _JSTestSelector),
}


def filter_tests(test_kind, selector_config, test_file_explorer=_DEFAULT_TEST_FILE_EXPLORER):
    """Filters the tests according to a specified configuration.

    Args:
        test_kind: the test kind, one of 'cpp_integration_test', 'cpp_unit_test', 'db_test',
            'json_schema_test', 'js_test'.
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
